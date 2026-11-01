/* face_recog_service.cpp - 人脸识别服务实现（S8-0 自检 + S8-2 常驻识别引擎）
 *
 * 设计依据：docs/项目规划/S8_人脸识别打卡系统方案.md §5.3 / §13 / §14.2
 *
 * ★★ 本文件最值得读的一段：`dl::Model` 有两种构造方式，**只有一种能用**。
 *
 *   官方 MFN（esp-dl/models/human_face_recognition/human_face_recognition.cpp）：
 *
 *       MFN::MFN(const char *model_name) {
 *           m_model = new dl::Model(sd_path.c_str(), fbs::MODEL_LOCATION_IN_SDCARD);
 *           m_model->minimize();                       // ← 无条件！
 *           m_image_preprocessor = new dl::image::FeatImagePreprocessor(m_model, ...);
 *           m_postprocessor      = new dl::feat::FeatPostprocessor(m_model);
 *       }
 *
 *   【崩溃点 A —— 官方写法自带的问题】`dl::Model` 的构造函数在 load 失败时
 *   只打日志、**不返空也不抛**，于是随后无条件的 `minimize()` →
 *   `fbs::FbsModel::clear_map()` 是一次空指针解引用 —— 这正是本工程 §61 记录的
 *   POSE 崩溃（`dl::Model::minimize() → fbs::FbsModel::clear_map()`）。
 *
 *   【崩溃点 B —— "自己改装拿返回值"时的陷阱，2026-09-28 上机实测踩到】
 *   为了拿到 load 的 esp_err_t，很自然会想到"默认构造 + 手动 load"：
 *
 *       dl::Model *m = new dl::Model();                     // ✗ 空构造！
 *       esp_err_t err = m->load(path, LOCATION_IN_SDCARD, nullptr);
 *
 *   **这条路必崩**，四条证据链：
 *     · `Model() {}` 是**空构造**（dl_model_base.hpp:62），它不创建 model_context；
 *     · `m_model_context` 的成员初值是 `nullptr`（同文件 :52）；
 *     · 而**所有带参构造**的第一行都是 `m_model_context = new ModelContext();`
 *       （dl_model_base.cpp:26 / 43 / 60 / 76 —— 四个重载无一例外）；
 *     · `load(fbs::FbsModel*)` 一进来（dl_model_base.cpp:145）就
 *       `m_model_context->clear()` ⇒ **空指针解引用**。
 *   实测 panic（与 §61 是**两个不同的崩溃点**，别混为一谈）：
 *       Guru Meditation / Load access fault
 *       → dl::Model::load(fbs::FbsModel*)  dl_model_base.cpp:145
 *         → dl::ModelContext::clear()  反汇编码停在 stl_vector.h:1119 `std::vector::size()`
 *       MTVAL = 0x0（就是解引用空指针）
 *
 *   ⇒ **正确做法（本文件采用）**：用**带参构造**建对象，再从**产物**判成败：
 *
 *       dl::Model *m = new dl::Model(path, fbs::MODEL_LOCATION_IN_SDCARD);
 *       if (m->get_fbs_model() == nullptr ||
 *           m->get_input()  == nullptr ||          // build() 的产物
 *           m->get_output() == nullptr) { ... }    // load 失败 ⇒ build 不会执行
 *       m->minimize();                              // 到这里才安全
 *
 *   为什么这三个判据本身不会崩：`get_input()`/`get_output()` 内部都有
 *   `if (m_inputs.empty()) { log; return nullptr; }`（dl_model_base.cpp:373 / 412），
 *   而带参构造是 `if (load(...) == ESP_OK) build(...);`（:26）⇒ load 失败时
 *   build 不执行、容器为空 ⇒ 它们**安静地返回 nullptr**，正是我们要的信号。
 *
 * ★ 第二个坑（复刻别人的类时最容易漏）：`dl::feat::FeatImpl` **没有构造函数**，
 *   它的三个成员（m_model / m_image_preprocessor / m_postprocessor）是**未初始化的垃圾值**，
 *   而 `~FeatImpl()` 会无条件 delete 这三个指针 ⇒ 构造中途失败时析构会 delete 垃圾指针。
 *   ⇒ 必须在构造函数**第一件事**就把三个指针全部置 nullptr。
 *
 * ★★ 第三个坑（S8-2 新增，**MSR/MNP 那一侧没有"产物判据"可用**）：
 *   `human_face_detect::MSRMNP` 的构造是 `m_msr(name,...)` / `m_mnp(name,...)`，
 *   而 `MSR::MSR()` 内部同样是"`new dl::Model(...)` 之后**无条件** `minimize()`"
 *   （human_face_detect.cpp:19-26）⇒ 它就落在崩溃点 A 上，而且**没有任何非侵入的办法
 *   从外部判断它是否构建成功**（`detect_wrapper` 的 `m_model` 永远非空）。
 *   ⇒ 唯一的对策是**在构造之前把失败原因全部拦掉**（文件在位 + 内存够），
 *     这正是 `engine_mem_ok()` 存在的意义：**内存不够这一情况绝不能交给 esp-dl 去失败**。
 *
 * ★★ 第四个坑（2026-11-01 打卡崩溃的根因，**必读**）：PSRAM 栈任务里**碰 flash** 必崩。
 *
 *   本工程把两条任务（加载 / 推理）的栈挪到了 PSRAM（内部 RAM 实在凑不出 10 KB），
 *   于是 `stat()` / `fopen()` / `fread()` 一旦在这些任务里执行，就会走到
 *       esp_partition_read → esp_flash_read → spi_flash guard
 *       → spi_flash_disable_interrupts_caches_and_other_cpu()
 *   ⇒ 该函数里的断言 `esp_task_stack_is_sane_cache_disabled()`
 *      （= `!esp_stack_ptr_in_extram(esp_cpu_get_sp()))` 直接 abort：
 *       `assert failed: spi_flash_disable_interrupts_caches_and_other_cpu cache_utils.c:114`
 *   真机证据（2026-11-01）：`starting recog load task (PSRAM)` 之后**一行 `[recogmem]`
 *   都没打出来** ⇒ 崩在**第一个** flash 访问，也就是 `stat(RECOG_MFN_PATH)` 上。
 *
 *   ⚠ 本工程 `CONFIG_BSP_SD_MOUNT_POINT == "/littlefs"`（**不是 SD 卡**！），
 *     `CONFIG_HUMAN_FACE_DETECT_MODEL_IN_SDCARD=y` ⇒ 三个 .espdl 全在 littlefs，
 *     也就是说**读它们 = 读 flash**。旧注释里"FATFS 读不关 flash cache"的判据在这里不成立。
 *
 *   ⇒ 本文件的纪律（别再改回去）：
 *     ① **所有** flash 访问（stat/open/read，含"文件多大"这种小事）必须在
 *         `read_file_to_psram()` 里做 —— 它在一条**内部 RAM 栈**的临时任务上跑；
 *     ② PSRAM 栈任务只做纯内存/计算工作：用 `read_file_to_psram()` 交回的
 *        **PSRAM 缓冲**建模型（`MODEL_LOCATION_IN_FLASH_RODATA` 在这里等价于
 *        "手工管理内存缓冲"，esp-dl 只警告 `fbs model is copied to PSRAM`，不读 flash）；
 *     ③ 缓冲的寿命必须 ≥ 模型：`param_copy=false` 时模型权重直接指向该缓冲
 *        （esp-dl 的 `auto_free=false`），所以**先 delete 模型，再 heap_caps_free(缓冲)**。
 */

#include "face_recog_service.h"
#include "mem_diag.h"                          /* mem_diag_snap()：加载前后的 PSRAM 现场 */

#include "dl_detect_base.hpp"                  /* dl::detect::DetectImpl（MSR 用它跑） */
#include "dl_detect_msr_postprocessor.hpp"     /* MSRPostprocessor                    */
#include "dl_detect_mnp_postprocessor.hpp"     /* MNPPostprocessor                    */
#include "dl_image_preprocessor.hpp"           /* dl::image::ImagePreprocessor        */

#include "dl_feat_base.hpp"                    /* dl::feat::FeatImpl                  */
#include "dl_feat_image_preprocessor.hpp"      /* dl::image::FeatImagePreprocessor    */
#include "dl_feat_postprocessor.hpp"           /* dl::feat::FeatPostprocessor         */
#include "dl_model_base.hpp"                   /* dl::Model, fbs::MODEL_LOCATION_*    */
#include "dl_image_define.hpp"                 /* dl::image::img_t / PIX_TYPE         */
#include "dl_tensor_base.hpp"                  /* dl::TensorBase                      */

#include "mem/static_pool.h"                   /* ★ 编译期静态内部池（recog_read 栈） */

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"                   /* 读文件工的互斥（防两个调用方互相覆盖工位） */
#include "freertos/idf_additions.h"            /* xTaskCreatePinnedToCoreWithCaps / vTaskDeleteWithCaps */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <list>
#include <vector>

static const char *TAG = "RECOG";

/* ---------------------------------------------------------------------------
 *  常量
 * ------------------------------------------------------------------------- */

/** 模型目录：与 FACE/HAND 档同目录。
 *  ★ 不要写死 "/littlefs" —— 用 BSP 的挂载点宏，挂载点若变则**编译期**跟着变
 *    （face_service.cpp 的 FACE_MODEL_DIR 也是这个写法）。 */
#define RECOG_MFN_FILE    "human_face_feat_mfn_s8_v1.espdl"
#define RECOG_MSR_FILE    "human_face_detect_msr_s8_v1.espdl"
#define RECOG_MNP_FILE    "human_face_detect_mnp_s8_v1.espdl"

/** ★★ 这里必须用**常量数组**，不能用宏 —— 踩过的坑：
 *   写成 `#define RECOG_MFN_PATH (DIR "/" FILE)` 时，展开结果是 ("a" "/" "b") 这种
 *   **带括号的表达式**，一旦放进 ESP_LOGI 的 format 位置，会被 ESP-IDF 的
 *   `LOG_FORMAT()` 的字符串字面量拼接技巧拒绝，报
 *       error: expected ')' before string constant
 *   而报错位置在 esp_log.h 里，极难定位到是本行宏的锅。
 *   常量数组是真·字面量拼接连结（编译期完成），任何位置都能用。 */
static const char RECOG_MFN_PATH[] =
    CONFIG_BSP_SD_MOUNT_POINT "/" CONFIG_HUMAN_FACE_DETECT_MODEL_SDCARD_DIR "/" RECOG_MFN_FILE;

/** MFN 输入边长（写死 112：模型内部第一层就要求 112x112x3） */
#define RECOG_INPUT_W     112
#define RECOG_INPUT_H     112

/** ★ 引擎支持的最大帧尺寸。**这个数与帧缓冲的分配直接绑定**：
 *   240x240x3 = 172,800 B。UI 侧（tab_camera）的预览画布就是 240x240。
 *   若要支持更大的帧（比如将来 320x240），改这里 + 重新核算 PSRAM 即可。 */
#define RECOG_MAX_W       240
#define RECOG_MAX_H       240
#define RECOG_FRAME_BYTES (RECOG_MAX_W * RECOG_MAX_H * 3)

/** 单文件连续块余量：模型之外还有中间张量（见 §61 的 model_mem_ok 口径） */
#define RECOG_ALLOC_SLACK (64 * 1024)
/** 总量余量：三个模型 + 帧缓冲 + 中间张量 */
#define RECOG_TOTAL_SLACK (256 * 1024)

/** ★★★ MFN 加载峰值里那块 **arena** 的估算值（字节）。
 *
 *  SDCARD 模式下**两块连续 PSRAM 同时存在**（这正是本工程 P0-A 要治的东西）：
 *    ① `fbs_loader.cpp:311` `dl::tool::malloc_aligned(size, MALLOC_CAP_DEFAULT)`
 *       —— 整份 .espdl 读进一块连续 RAM（MFN = 1,295,200 B）；
 *    ② 随后 `ModelContext::root_alloc()`（`dl_model_context.cpp:155-181`）再要一块
 *       `m_psram_root`（MFN 的 arena），§14 的实测口径 ≈ 820,750 B。
 *  ⇒ 真实峰值 ≈ 1,295,200 + 820,750 = **2,115,950 B**，而不是最大单文件的 1,295,200 B。
 *
 *  ⚠ 这是**估算**：arena 的真实大小由 esp-dl 的 model info 在运行期决定（且随模型变），
 *    所以它**只用于报警**，不参与"拒绝加载"的判据 —— 拒绝线仍按"最大单文件 + slack"
 *    算，免得把本来加载得成的场景挡掉（本工程一贯口径：先让它试，失败必须留痕）。
 *  ⚠ 换成 `MODEL_LOCATION_IN_FLASH_PARTITION` 后 ① 会整个消失 ⇒ 峰值退化成 arena 本身。
 *    这正是 P0-A 值得做的量化理由，也是本常量将来可以删掉的时点。 */
#define RECOG_PEAK_ARENA_EST  (820750u)

/** 识别任务栈（内部 RAM）。
 *  ★ 必须在内部 RAM：它在 esp-dl 的加载路径上会读 littlefs（flash）,
 *    PSRAM 栈会在 flash cache 关闭窗口里崩（F35/F38/F40）。
 *  ⚠ 这个数是**估计值**：任务在自删之前会把真实水位
 *    （`uxTaskGetStackHighWaterMark`）打进日志 —— 首次上机后按那条日志调，
 *    不要再靠猜（本工程 taskLVGL 的栈就是这样一路量出来的）。 */
#define RECOG_TASK_STACK   8192
#define RECOG_TASK_PRIO    4
/** ★ 钉在 Core0：Core1 是 LVGL + cam_task 的地盘，识别绝不能去抢它们的核。 */
#define RECOG_TASK_CORE    0

/** 一次性**加载**任务栈（内部 RAM，理由同 RECOG_TASK_STACK）。
 *  ★★ 2026-09-28 上机事故后从 8192 明确成 10240，并把"谁来加载"钉死在这条任务上：
 *   ① cam_task 只有 CAM_TASK_STACK=3584 B，而本次上机日志自己的报表就是
 *      `[stack] cam size=3584 used=2540 hwm=1044 need=4096` —— 它**已经不够了**，
 *      连 face_service 那个 495 KB 的模型都只是勉强跑（P2-5 那个问号至今没销）；
 *   ② LVGL 线程栈 9728、used 6688、hwm 3040 —— 同样放不下，而且会让 UI 卡约 2 s；
 *   ③ 当年 S8-0 探针就是用 **10240 B 的专用任务**跑通这条链路并打出 PROBE PASS 的
 *      ⇒ 10240 是**实测过的数**，不是估的。 */
#define RECOG_LOAD_STACK   10240

/** ★ 2026-11-01 真机：`xTaskCreate(recog_load) FAILED (internal RAM tight, stack=10240)`
 *  —— 内部 RAM 只剩 ~27 KB 且最大连续块 8704 B，10 KB 内部栈**凑不出来**，
 *  所以模型永远加载不了（打卡一直 state=0）。
 *  改为把栈放 PSRAM（`xTaskCreatePinnedToCoreWithCaps`），TCB 仍由内部 RAM 分配。
 *  ⚠ 改了 PSRAM 栈之后**本任务里一行 flash 访问都不许有**（stat/fopen/fread 全算）——
 *    2026-11-01 的打卡崩溃就是这么来的，见文件头"第四个坑"。
 *    flash 访问统一交给 `read_file_to_psram()`（内部 RAM 栈的临时任务）。
 *  任务用 PSRAM 栈后，自删必须用 `vTaskDeleteWithCaps(NULL)`，
 *  否则这条栈会用普通 free 回收（caps 不匹配）。 */
#define RECOG_TASK_MEM_CAPS   (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

/** 读模型文件的**临时任务**栈（★ 必须内部 RAM）。
 *  它只做 stat + fopen + fread 三件事，所以能吃下小栈：内部 RAM 在打卡会话里
 *  只剩 ~7 KB（日志 `INT free=7259 largest=7168`），10240/8192 都凑不出来，
 *  4096 是"能建起来 + 够 stdio/littlefs 调用深度"的折中（真机实测值）。
 *  ★ 这条任务**不能**用 PSRAM 栈：它整个存在意义就是"在内部栈上碰 flash"。 */
#define RECOG_READ_STACK   4096
#define RECOG_READ_PRIO    RECOG_TASK_PRIO
#define RECOG_READ_CORE    RECOG_TASK_CORE

/* ---------------------------------------------------------------------------
 *  MFN —— 复刻官方 human_face_recognition::MFN（修正见文件头）
 * ------------------------------------------------------------------------- */
namespace {

class MFN : public dl::feat::FeatImpl {
public:
    /** @param buf  由 `read_file_to_psram()` 读进 PSRAM 的整份 .espdl（寿命 ≥ 本对象） */
    explicit MFN(const uint8_t *buf)
    {
        /* ★ 见文件头"第二个坑"：三个基类成员此刻是垃圾值，先全部归零，
         *   否则构造失败时 ~FeatImpl() 会 delete 野指针。 */
        m_model              = nullptr;
        m_image_preprocessor = nullptr;
        m_postprocessor      = nullptr;

        /* ★★ 必须用**带参构造**：`Model()` 空构造不创建 m_model_context，
         *    而 load() 一进来就 `m_model_context->clear()` ⇒ 空指针 panic。
         *    完整证据链（四条）见文件头"崩溃点 B"。
         * ★ 这里传**内存缓冲**（不是路径）：`MODEL_LOCATION_IN_FLASH_RODATA` 对
         *    esp-dl 的含义就是"直接拿这个指针当模型数据"，不做任何 flash IO；
         *    指针若在 PSRAM，它会打一条 `fbs model is copied to PSRAM` 的 WARN
         *    并置 `auto_free=false`（⇒ 缓冲由本文件管，见 engine_unload 的顺序）。 */
        dl::Model *m = new dl::Model(reinterpret_cast<const char *>(buf),
                                     fbs::MODEL_LOCATION_IN_FLASH_RODATA);
        if (m == nullptr) {
            return;
        }

        /* ★★ 带参构造**没有返回值**（它内部是 `if (load(...)==ESP_OK) build(...);`），
         *    所以"装上了没有"只能从**产物**上看：
         *      · get_fbs_model()        —— 模型文件本身解析出来没有；
         *      · get_input()/get_output() —— build() 的产物（load 失败时 build 不执行）。
         *    这三个 getter 内部**自带判空**（空了就 log + return nullptr），
         *    所以拿它们当判据不会引入新的崩溃。
         *    ★ 判据不过就 return，**绝不让 minimize() 执行** —— 那才是 §61 的崩法。 */
        if (m->get_fbs_model() == nullptr ||
            m->get_input()     == nullptr ||
            m->get_output()    == nullptr) {
            ESP_LOGE(TAG, "MFN not built (fbs=%p in=%p out=%p) -> refuse minimize()",
                     (void *)m->get_fbs_model(), (void *)m->get_input(),
                     (void *)m->get_output());
            delete m;
            return;
        }

        m->minimize();                           /* 到这里才是安全的 */
        m_model = m;

        /* 均值/标准差 127.5、rgb_swap=true —— 与官方逐字一致，**不要"顺手优化"**：
         * MFN 训练时的归一化就是这个，改了会静默降低准确率（不报错，只是认不准）。 */
        m_image_preprocessor = new dl::image::FeatImagePreprocessor(
            m_model, {127.5f, 127.5f, 127.5f}, {127.5f, 127.5f, 127.5f}, true);
        m_postprocessor = new dl::feat::FeatPostprocessor(m_model);
    }

    /** 三个成员都就绪才算真可用（缺一个就调 run() 会空指针） */
    bool ok() const
    {
        return m_model != nullptr && m_image_preprocessor != nullptr && m_postprocessor != nullptr;
    }
};

/** 检测模型的"产物判据"（理由见文件头"崩溃点 A"）：
 *  `dl::Model` 带参构造在 load 失败时**不会**把对象变空，只能看 build 的产物。 */
inline bool model_built(dl::Model *m, const char *what)
{
    if (m != nullptr && m->get_fbs_model() != nullptr &&
        m->get_input() != nullptr && m->get_output() != nullptr) {
        return true;
    }
    ESP_LOGE(TAG, "%s not built (fbs=%p) -> refuse minimize()",
             what, m ? (void *)m->get_fbs_model() : nullptr);
    return false;
}

/* ---------------------------------------------------------------------------
 *  MSR + MNP（检出 + 5 点）—— **复刻 vendor 组件的两个类，只把模型来源改成缓冲**
 *
 *  为什么要复刻而不是继续 `HumanFaceDetect(MSRMNP_S8_V1)`：
 *  组件在 `CONFIG_HUMAN_FACE_DETECT_MODEL_IN_SDCARD=y` 下**内部自已 fopen**
 *  （human_face_detect.cpp:22-24 / 43-45），而本工程那条路径落在 littlefs 上 ⇒
 *  在 PSRAM 栈任务里必崩（文件头第四个坑）。组件是 managed_components（gitignore），
 *  本地改会在重装组件时丢失 ⇒ 把这两个类搬到这里，模型从**内存缓冲**建。
 *
 *  ⚠ 数值参数（0.5/0.5、top_k=10、anchors）与 human_face_detect.cpp **逐字一致**，
 *    换模型时必须以 vendor 组件的同版本为准；自己"顺手调"会让检出行为悄悄漂移。
 * ------------------------------------------------------------------------- */
class MsrBuf : public dl::detect::DetectImpl {
public:
    bool ok_ = false;

    explicit MsrBuf(const uint8_t *buf)
    {
        /* ★ DetectImpl 三个成员同样是垃圾值，而 ~DetectImpl() 无条件 delete ⇒ 先归零。
         *   （与 MFN 的"第二个坑"同一个理由。） */
        m_model              = nullptr;
        m_image_preprocessor = nullptr;
        m_postprocessor      = nullptr;

        dl::Model *m = new dl::Model(reinterpret_cast<const char *>(buf),
                                     fbs::MODEL_LOCATION_IN_FLASH_RODATA);
        if (!model_built(m, "MSR")) {
            delete m;
            return;
        }
        m->minimize();
        m_model = m;
        m_image_preprocessor = new dl::image::ImagePreprocessor(m_model, {0, 0, 0}, {1, 1, 1}, true);
        m_postprocessor = new dl::detect::MSRPostprocessor(
            m_model, m_image_preprocessor, 0.5f, 0.5f, 10,
            {{8, 8, 9, 9, {{16, 16}, {32, 32}}}, {16, 16, 9, 9, {{64, 64}, {128, 128}}}});
        ok_ = true;
    }
};

/** MNP（5 点精修）：不是 Detect 的子类，run() 的写法照抄 human_face_detect.cpp:77-111 */
class MnpBuf {
public:
    bool ok_ = false;
    dl::Model                        *m_model = nullptr;
    dl::image::ImagePreprocessor     *m_image_preprocessor = nullptr;
    dl::detect::MNPPostprocessor     *m_postprocessor = nullptr;

    explicit MnpBuf(const uint8_t *buf)
    {
        dl::Model *m = new dl::Model(reinterpret_cast<const char *>(buf),
                                     fbs::MODEL_LOCATION_IN_FLASH_RODATA);
        if (!model_built(m, "MNP")) {
            delete m;
            return;
        }
        m->minimize();
        m_model = m;
        m_image_preprocessor = new dl::image::ImagePreprocessor(m_model, {0, 0, 0}, {1, 1, 1}, true);
        m_postprocessor = new dl::detect::MNPPostprocessor(
            m_model, m_image_preprocessor, 0.5f, 0.5f, 10, {{1, 1, 0, 0, {{48, 48}}}});
        ok_ = true;
    }

    ~MnpBuf()
    {
        delete m_model;
        delete m_image_preprocessor;
        delete m_postprocessor;
    }

    std::list<dl::detect::result_t> &run(const dl::image::img_t &img,
                                         std::list<dl::detect::result_t> &candidates)
    {
        m_postprocessor->clear_result();
        for (auto &candidate : candidates) {
            int center_x = (candidate.box[0] + candidate.box[2]) >> 1;
            int center_y = (candidate.box[1] + candidate.box[3]) >> 1;
            int side = DL_MAX(candidate.box[2] - candidate.box[0],
                              candidate.box[3] - candidate.box[1]);
            candidate.box[0] = center_x - (side >> 1);
            candidate.box[1] = center_y - (side >> 1);
            candidate.box[2] = candidate.box[0] + side;
            candidate.box[3] = candidate.box[1] + side;
            candidate.limit_box(img.width, img.height);

            m_image_preprocessor->preprocess(img, candidate.box);
            m_model->run();
            m_postprocessor->postprocess();
        }
        m_postprocessor->nms();
        return m_postprocessor->get_result(img.width, img.height);
    }
};

/** MSR → MNP 的组合（等价于 human_face_detect::MSRMNP::run），只从缓冲建。 */
class DetBuf {
public:
    MsrBuf *msr = nullptr;
    MnpBuf *mnp = nullptr;

    ~DetBuf()
    {
        delete msr;
        delete mnp;
    }

    bool ok() const { return msr != nullptr && msr->ok_ && mnp != nullptr && mnp->ok_; }

    std::list<dl::detect::result_t> &run(const dl::image::img_t &img)
    {
        std::list<dl::detect::result_t> &candidates = msr->run(img);
        return mnp->run(img, candidates);
    }
};

/** 四舍五入到 int（landmarks 是 vector<int>，而标准 5 点是小数坐标） */
inline int iround(float v)
{
    return (int)(v + (v >= 0.0f ? 0.5f : -0.5f));
}

/** ★★ 标准五点布局 —— **顺序是 esp-dl 写死的，不是直觉顺序**。
 *
 *  `dl_feat_image_preprocessor.cpp` 里那份 `s_std_ldks_112` 的实际顺序是：
 *      左眼 → 左嘴角 → 鼻尖 → 右眼 → 右嘴角
 *  而 MNP (`dl_detect_mnp_postprocessor.cpp`) 输出的 `result_t.keypoint`
 *  正好与之一致 ⇒ 真实识别路径上必须**原样透传**，一个都不许重排。
 *
 *  ★ 本数组只给 S8-0 探针（合成图）用 —— 它只验证"能不能推理"，不验证准确率，
 *    所以这里把顺序**按真实布局订正**，避免后来人照抄这里去写正式逻辑时踩坑。 */
const float kLandmark112[10] = {
    38.2946f, 51.6963f,   /* 0 左眼   */
    41.5493f, 92.3655f,   /* 1 左嘴角 */
    56.0252f, 71.7366f,   /* 2 鼻尖   */
    73.5318f, 51.5014f,   /* 3 右眼   */
    70.7299f, 92.2041f,   /* 4 右嘴角 */
};

/** 合成一张 112x112 RGB888 渐变图（仅 S8-0 自检用）。
 *  ★ S8-0 **不验证准确率**，只验证"能不能推理"，所以不找真人脸。
 *    用平滑渐变（而非纯色/随机噪声）是为了让中间特征图不至于全零或爆掉。 */
void make_synthetic_face(uint8_t *buf, int w, int h)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t *p = buf + (y * w + x) * 3;
            p[0] = (uint8_t)(x * 2);      /* R 横向渐变 */
            p[1] = (uint8_t)(y * 2);      /* G 纵向渐变 */
            p[2] = (uint8_t)128;          /* B 恒定 */
        }
    }
}

} // namespace

/* ---------------------------------------------------------------------------
 *  引擎状态（S8-2）
 * ------------------------------------------------------------------------- */
namespace {

/* 三件套：MSR+MNP（检出 + 5 点）与 MFN（特征） */
DetBuf          *s_det = nullptr;
MFN             *s_mfn = nullptr;

/** 三个模型的内存缓冲（PSRAM）。**寿命 ≥ 对应模型**：
 *  esp-dl 用 `param_copy=false` 时权重直接指向这里，所以卸载顺序必须是
 *  "先 delete 模型 → 再 heap_caps_free 缓冲"（见 engine_unload）。 */
uint8_t *s_msr_buf = nullptr;
uint8_t *s_mnp_buf = nullptr;
uint8_t *s_mfn_buf = nullptr;

/** RGB888 帧缓冲（PSRAM）。**唯一的大缓冲**，由 submit() 在 LVGL 线程里填写。 */
uint8_t *s_frame = nullptr;
int      s_frame_w = 0;
int      s_frame_h = 0;

/** 结果快照（PSRAM 分配：约 2.2 KB，不占内部 .bss） */
recog_result_t *s_result = nullptr;

face_state_t s_state = FACE_IDLE;
bool         s_load_pending = false;
/** ★ volatile：加载任务收尾时要读它（"加载期间有没有人按关"），见 recog_load_task。 */
volatile bool s_enabled = false;
int64_t      s_load_req_us = 0;

volatile bool s_job_running = false;   /* 任务在跑 */
volatile bool s_result_new  = false;   /* 有未消费的结果 */
volatile bool s_job_is_extract = false;/* 当前作业是否"只提特征不查库" */
volatile bool s_load_running = false;  /* ★ 一次性加载任务在跑（stop() 靠它防 UAF） */
/** ★ "拒绝卸载"只报一次：宿主是**每 50 ms 重试**的（recog_pump 的写法），
 *  不加这条会把 2 s 的等待刷成 40 行同样的 WARN —— 与 §16.9.1 限频日志同一个理由。 */
bool          s_stop_defer_logged = false;
uint32_t      s_last_ms = 0;

/* ★ 关键点自检的累计计数（方案 §8-S8-0 "5 点是否稳定"的量化判据）。
 *   每次识别 ++s_runs，异常时 ++s_kp_bad，收尾日志里一起打出来 ——
 *   "跑了 200 次、异常 0 次"比"看着好像没问题"强得多。 */
uint32_t      s_runs   = 0;
uint32_t      s_kp_bad = 0;

/** 看门狗：请求后多久**还没开始**加载就算失败。
 *  ★ 语义（2026-09-28 收窄）：判定的是"`face_recog_poll()` 有没有被周期调用"，
 *    不是"加载慢不慢"（加载本身约 1.9 s，由加载任务自己报耗时）。
 *    所以条件里带 `!s_load_running` —— 任务一旦起来，这条就不该再叫。 */
#define RECOG_LOAD_TIMEOUT_US (8000 * 1000)

/** 从 SD/littlefs 路径拼一个常量数组（同 RECOG_MFN_PATH 的理由） */
const char RECOG_MSR_PATH[] =
    CONFIG_BSP_SD_MOUNT_POINT "/" CONFIG_HUMAN_FACE_DETECT_MODEL_SDCARD_DIR "/" RECOG_MSR_FILE;
const char RECOG_MNP_PATH[] =
    CONFIG_BSP_SD_MOUNT_POINT "/" CONFIG_HUMAN_FACE_DETECT_MODEL_SDCARD_DIR "/" RECOG_MNP_FILE;

/* ===========================================================================
 *  模型文件读取 —— ★ **本文件唯一允许碰 flash 的地方**（文件头第四个坑）
 * =========================================================================== */
struct read_job_t {
    const char  *path;
    uint8_t     *buf;          /* 出参：PSRAM 缓冲（stat_only 时保持 nullptr） */
    size_t       size;         /* 出参：文件字节数（0 = 不存在/空） */
    esp_err_t    err;
    bool         stat_only;    /* true：只 stat 拿尺寸，不分配、不读 */
    TaskHandle_t waiter;
};

/** 当前工位（指向调用方栈上的 read_job_t）。worker 只在拿到通知后读它一次。 */
static read_job_t *volatile s_read_job = nullptr;

/** ★★ 常驻"内部栈读文件工"（**永不退出**，2026-11-01 第二版）。
 *
 *  ★ 为什么从"一次性任务"改成"常驻"（真机崩溃的根因，别再改回去）：
 *    静态任务的栈/TCB 是调用方给的、**FreeRTOS 不会回收**。一次性任务
 *    `vTaskDelete(NULL)` 之后仍挂在 `xTasksWaitingTermination` 上等 idle 清理，
 *    而本调用点**紧接着就要读下一个文件**（MSR → MNP → MFN 是连续三次）。
 *    第二次 create 复用同一个 `StaticTask_t` 时，前一个实例还没被清出链表
 *    ⇒ `prvInitialiseNewTask` 覆写 `xStateListItem` ⇒ 链表被写坏
 *    ⇒ idle 在 `prvCheckTasksWaitingTermination` 里 `uxListRemove` 崩
 *    （实测：`Core 1 panic Load access fault`，MEPC = `list.c:207 uxListRemove`，
 *      RA = `tasks.c:4598 prvCheckTasksWaitingTermination`）。
 *  ⇒ 唯一安全的形态：**建一次、永不删、用任务通知交作业**。
 *
 *  这里**不能**把栈放 PSRAM —— 整个函数的存在意义就是"在内部栈上碰 flash"。
 */
void read_file_task(void *arg)
{
    (void)arg;

    for (;;) {
        /* 等工位：调用方先写好 s_read_job，再 xTaskNotifyGive */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        read_job_t  *j      = s_read_job;
        TaskHandle_t waiter = j->waiter;   /* ★ 先拷出来：收尾时不能再碰 j 的字段 */

        struct stat st;
        if (stat(j->path, &st) != 0 || st.st_size <= 0) {
            j->err = ESP_ERR_NOT_FOUND;
        } else {
            j->size = (size_t)st.st_size;
            if (j->stat_only) {
                j->err = ESP_OK;
            } else {
                /* 64 B 对齐：esp-dl 的 `param_copy` 判据要求 16 B 对齐（fbs_loader.cpp:345），
                 * 否则它会退化成"把权重再拷一份到 PSRAM"（多占一整份模型内存）。 */
                j->buf = (uint8_t *)heap_caps_aligned_alloc(64, j->size,
                                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (j->buf == nullptr) {
                    j->err = ESP_ERR_NO_MEM;
                } else {
                    FILE *f = fopen(j->path, "rb");
                    if (f == nullptr) {
                        j->err = ESP_ERR_NOT_FOUND;
                    } else {
                        size_t rd = fread(j->buf, 1, j->size, f);
                        fclose(f);
                        /* 读少了 = 文件在 读的过程中被改，或 littlefs 出错 —— 两种都不能拿去建模型 */
                        j->err = (rd == j->size) ? ESP_OK : ESP_FAIL;
                    }
                }
                if (j->err != ESP_OK && j->buf != nullptr) {
                    heap_caps_free(j->buf);
                    j->buf = nullptr;
                }
            }
        }

        /* ★ 只通知、不再碰 j 的任何字段（j 在调用方栈上，那边收到通知就会返回）。 */
        xTaskNotifyGive(waiter);
    }
}

/**
 * @brief 把**一个**模型文件读进 PSRAM 缓冲（或只量尺寸）。
 * @param stat_only  true = 只要尺寸（`*out_buf` 保持 nullptr）
 * @return true = 成功（stat_only 时表示文件存在且非空）
 *
 * ★ 2026-11-01 起栈来自**编译期静态池**（`static_pool.h`）：池块在 .bss 里预先划好，
 *   所以"内部 RAM 只剩 ~7 KB、最大连续块 7 KB"时这条任务**仍然建得起来**。
 *   调用者仍可以在 PSRAM 栈任务里调本函数 —— flash 访问都在内部栈的 read_file_task 里。
 */
bool read_file_to_psram(const char *path, bool stat_only, uint8_t **out_buf, size_t *out_size)
{
    /* ★ 栈来自**编译期静态池**（static_pool.h），不再向内部堆要 4 KB：
     *   打卡会话里 `INT free=7259 largest=7168` 的时候，动态 4 KB 栈是"碰运气"，
     *   而读模型是打卡的第一步 —— 这里失败就等于整个人脸功能不可用。
     *   池块终生绑定 "recog_read"（只申请一次，之后每次加载复用），
     *   所以反复加载/卸载模型不会产生任何内部堆分配/释放。
     *   ★ TCB 也静态（FreeRTOS 静态任务要求调用方提供），两者都不会被回收
     *     ⇒ 这条任务**只建一次、永不退出**（原因见 read_file_task 的头注释）。 */
    static StackType_t      *s_read_stack  = nullptr;
    static StaticTask_t      s_read_tcb;
    static TaskHandle_t      s_read_worker = nullptr;
    static SemaphoreHandle_t s_read_lock   = nullptr;

    if (s_read_worker == nullptr) {
        if (s_read_stack == nullptr) {
            s_read_stack = (StackType_t *)static_pool_alloc(RECOG_READ_STACK, "recog_read");
            if (s_read_stack == nullptr) {
                ESP_LOGE(TAG, "static pool has no room for recog_read stack (%d B) | INT free=%u largest=%u",
                         RECOG_READ_STACK,
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
                return false;
            }
        }
        if (s_read_lock == nullptr) {
            s_read_lock = xSemaphoreCreateBinary();
            if (s_read_lock == nullptr) {
                ESP_LOGE(TAG, "xSemaphoreCreateBinary(recog_read) FAILED");
                return false;
            }
            xSemaphoreGive(s_read_lock);       /* 初始为"空闲" */
        }
        s_read_worker = xTaskCreateStaticPinnedToCore(read_file_task, "recog_read",
                                                      RECOG_READ_STACK, nullptr,
                                                      RECOG_READ_PRIO, s_read_stack,
                                                      &s_read_tcb, RECOG_READ_CORE);
        if (s_read_worker == nullptr) {
            ESP_LOGE(TAG, "xTaskCreateStatic(recog_read) FAILED");
            return false;
        }
        ESP_LOGI(TAG, "reader worker up (resident, internal stack %d B)", RECOG_READ_STACK);
    }

    read_job_t job;
    memset(&job, 0, sizeof(job));
    job.path      = path;
    job.stat_only = stat_only;
    job.err       = ESP_FAIL;
    job.waiter    = xTaskGetCurrentTaskHandle();
    *out_buf  = nullptr;
    *out_size = 0;

    /* 串行化：两个调用方同时进来会互相覆盖工位（worker 只有一个 s_read_job）。 */
    if (xSemaphoreTake(s_read_lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    s_read_job = &job;
    xTaskNotifyGive(s_read_worker);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   /* 读大文件要几百 ms，不能设超时 */
    xSemaphoreGive(s_read_lock);

    if (job.err != ESP_OK) {
        ESP_LOGE(TAG, "read %s FAILED: %s (%u B)", path, esp_err_to_name(job.err),
                 (unsigned)job.size);
        return false;
    }
    *out_buf  = job.buf;
    *out_size = job.size;
    return true;
}

/** 留一份 PSRAM 块分布（只在异常/高危路径调，正常路径零开销）。
 *  ★ 这是回答"到底能不能凑出连续块"的**唯一直接证据** ——
 *    largest 只是个数字，"哪几块、各多大、中间被谁卡着"才决定能不能合并出来。
 *
 *  ★★ 2026-09-28 改实现：**不再用 heap_caps_dump()**。
 *    它内部是 `multi_heap_dump()` → `multi_heap_internal_lock()`（**持堆锁**）
 *    → `tlsf_walk_pool()` 逐块 UART 打印。本工程实测一次打 1463 行 ⇒
 *    115200 波特下数秒，全程持堆锁 ⇒ 直接
 *    `Guru Meditation Error: Core 0 panic'ed (Interrupt wdt timeout on CPU0)`。
 *    ⇒ 改用 mem_diag_dump_top()：**锁内只收集、锁外才打印**（12 行），
 *      并且额外给出"最大的已占用块 + 地址" —— 那才是"谁占着 PSRAM"的答案。 */
void dump_psram_blocks(const char *why)
{
    mem_diag_dump_top(TAG, why);
}

/**
 * ★★ §61 的连续块预检（**必须在构造之前做**，原因见文件头"第三个坑"）。
 *
 * 判据两条（只看 free 总量会误判 —— esp-dl 在 SDCARD 模式把整个 .espdl
 * 读进**一块连续** RAM）：
 *   ① 连续块 ≥ **最大单文件** + slack（MFN 1.29 MB 是这里的大头）；
 *   ② 总量   ≥ 三个文件之和 + 帧缓冲 + slack。
 *
 * ★★ 2026-09-28 补第三条——**峰值线**（见 RECOG_PEAK_ARENA_EST 的推导）：
 *   上面两条只保证"最大单文件那次 malloc 拿得到"+"总量够"，但 SDCARD 模式的真实
 *   峰值是"文件缓冲 + arena **同时**存在" ⇒ **过了上面两条仍可能被 arena 分配打回**，
 *   症状就是 esp-dl 自己那句
 *       E esp-dl: Failed to alloc 802.10KB memory, largest available block size 761.50KB.
 *   历史上这条线没打，于是"过了预检却还是失败"看起来像灵异事件（§61/§67 各一次）。
 *   ⇒ 现在把它**提前**摆出来：过了硬线但低于峰值线时打 WARN + dump 分布，
 *     这样"到底还差多少、缺在谁身上"一次烧录就能读到。
 *   ⚠ 峰值线**只报警不拒绝**：它是估算值，用它拒绝会误伤。
 */
bool engine_mem_ok(size_t mfn_b, size_t msr_b, size_t mnp_b)
{
    size_t max_one = mfn_b;
    if (msr_b > max_one) max_one = msr_b;
    if (mnp_b > max_one) max_one = mnp_b;
    size_t sum = mfn_b + msr_b + mnp_b;

    size_t psram_free    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t int_largest   = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    size_t need_contig = max_one + RECOG_ALLOC_SLACK;
    size_t need_total  = sum + RECOG_FRAME_BYTES + RECOG_TOTAL_SLACK;
    size_t need_peak   = max_one + RECOG_PEAK_ARENA_EST + RECOG_ALLOC_SLACK;

    /* ★★ 两笔账**都打**（不管过不过）—— 这是本模块最重要的诊断行。
     *   hard  = 硬线（能否让 esp-dl 安全走到 minimize）
     *   peak  = 峰值线（真实能否跑起来）
     *   两条线分开报，才能一眼看出"是过不了硬线"还是"过了硬线但差在峰值"。 */
    ESP_LOGI(TAG, "[recogmem] PSRAM largest=%uKB free=%uKB | need contig=%uKB total=%uKB | "
                  "peak=%uKB (=file %uKB + arena~%uKB) | INT largest=%uKB",
             (unsigned)(psram_largest / 1024), (unsigned)(psram_free / 1024),
             (unsigned)(need_contig / 1024), (unsigned)(need_total / 1024),
             (unsigned)(need_peak / 1024), (unsigned)(max_one / 1024),
             (unsigned)(RECOG_PEAK_ARENA_EST / 1024), (unsigned)(int_largest / 1024));

    if (psram_largest >= need_contig && psram_free >= need_total) {
        /* 硬线过了 —— 但**这不代表跑得起来**（见函数头第三条）。 */
        if (psram_largest < need_peak) {
            ESP_LOGW(TAG, "[recogmem] ★ 过了硬线但**低于峰值线**：largest=%uKB < peak=%uKB "
                          "(差 %dKB) ⇒ MFN 很可能仍报 'Failed to alloc ... largest available "
                          "block size ...'。两个治本方向："
                          "① 换 MODEL_LOCATION_IN_FLASH_PARTITION（权重免拷贝，峰值降为 arena）；"
                          "② 加载前让识别引擎让位（拍照/录像时段）。",
                     (unsigned)(psram_largest / 1024), (unsigned)(need_peak / 1024),
                     (int)((need_peak - psram_largest) / 1024));
            dump_psram_blocks("peak line not met");
        }
        return true;
    }

    ESP_LOGE(TAG, "refuse recog load: need contiguous %uKB + %uKB total, "
                  "but PSRAM largest=%uKB free=%uKB | INT largest=%uKB",
             (unsigned)(need_contig / 1024), (unsigned)(need_total / 1024),
             (unsigned)(psram_largest / 1024), (unsigned)(psram_free / 1024),
             (unsigned)(int_largest / 1024));
    dump_psram_blocks("refused (below hard line)");
    return false;
}

/** 卸掉三件套 + 帧缓冲（幂等）。 */
void engine_unload(void)
{
    /* ★★ 顺序**不是**无关的：esp-dl 在内存缓冲模式下是 `auto_free=false`，
     *   模型的权重张量直接指向 s_*_buf ⇒ **必须先 delete 模型，再释放缓冲**，
     *   反过来就是"模型还在指一块已 free 的 PSRAM"。 */
    if (s_det) { delete s_det; s_det = nullptr; }
    if (s_mfn) { delete s_mfn; s_mfn = nullptr; }
    if (s_msr_buf) { heap_caps_free(s_msr_buf); s_msr_buf = nullptr; }
    if (s_mnp_buf) { heap_caps_free(s_mnp_buf); s_mnp_buf = nullptr; }
    if (s_mfn_buf) { heap_caps_free(s_mfn_buf); s_mfn_buf = nullptr; }
    if (s_frame) { heap_caps_free(s_frame); s_frame = nullptr; }
    s_frame_w = s_frame_h = 0;
}

/**
 * 真正的加载（★★ **只能由 recog_load_task 调用** —— 那条一次性任务。
 *
 * ★ 为什么不能在 cam_task 里同步加载（2026-09-28 才定下来）：cam_task 栈只有
 *   CAM_TASK_STACK=3584 B，上机报表自己写着 `used=2540 hwm=1044 need=4096`；
 *   也**不能**在 LVGL 线程里做（栈 9728/used 6688，且会卡 UI 约 2 s）。
 *
 * ★ 加载顺序：**先 MSRMNP（小，191 KB）后 MFN（大，1.29 MB）**。
 *   理由：MSR/MNP 的小分配不会破坏"大连续块"；随后 MFN 从仍然完整的大块里
 *   一次拿走 1.29 MB + slack。反过来先装 MFN 也不至于失败，但 §14.3 已实测
 *   "MFN 装卸一次会让 PSRAM largest 从 4.25 MB 掉到 2.94 MB"（不可合并的碎片），
 *   ⇒ **能晚一点制造碎片就晚一点**。
 */
void engine_load(void)
{
    /* ---- ⓪ 尺寸（★ 用内部栈任务去 stat：本任务栈在 PSRAM，碰 flash 必崩） ---- */
    size_t msr_b = 0, mnp_b = 0, mfn_b = 0;
    uint8_t *tmp = nullptr;
    if (!read_file_to_psram(RECOG_MSR_PATH, true, &tmp, &msr_b) |
        !read_file_to_psram(RECOG_MNP_PATH, true, &tmp, &mnp_b) |
        !read_file_to_psram(RECOG_MFN_PATH, true, &tmp, &mfn_b)) {
        /* 三种失败都会在 read_file_to_psram 里各自打印原因（NOT_FOUND / 任务建不起来） */
        s_state = FACE_FAIL;
        ESP_LOGE(TAG, "model size probe failed -> recog unavailable");
        return;
    }

    /* ★★ 加载起点的现场（诊断）：这是"这次到底有没有机会跑起来"的原始数据。
     *   与后面 `recog engine ready ... | PSRAM free=.. largest=..` 配对，
     *   两者之差就是**三个模型 + 帧缓冲实际吃掉的连续块** —— 有了这两个数，
     *   "能不能凑出来"就不再需要推算。 */
    {
        char who[96];
        snprintf(who, sizeof(who), "models MFN=%uKB MSR=%uKB MNP=%uKB",
                 (unsigned)(mfn_b / 1024), (unsigned)(msr_b / 1024), (unsigned)(mnp_b / 1024));
        mem_diag_snap(TAG, "recogmem pre", who);
    }

    if (mfn_b == 0 || msr_b == 0 || mnp_b == 0) {
        s_state = FACE_FAIL;
        ESP_LOGE(TAG, "model file missing/empty -> recog unavailable");
        ESP_LOGE(TAG, "  MFN %s : %u B", RECOG_MFN_PATH, (unsigned)mfn_b);
        ESP_LOGE(TAG, "  MSR %s : %u B", RECOG_MSR_PATH, (unsigned)msr_b);
        ESP_LOGE(TAG, "  MNP %s : %u B", RECOG_MNP_PATH, (unsigned)mnp_b);
        ESP_LOGE(TAG, "  -> S8-2 比 S8-0 多要两个文件；它们是 vendor 组件里的 "
                      "human_face_detect_msr/mnp_s8_v1.espdl，必须一起烧进 storage.bin");
        return;
    }
    if (!engine_mem_ok(mfn_b, msr_b, mnp_b)) {
        s_state = FACE_FAIL;
        return;
    }

    int64_t t0 = esp_timer_get_time();

    /* ---- ① 帧缓冲 + 结果快照（先分配，失败就直接退出，不碰模型） ---- */
    s_frame = (uint8_t *)heap_caps_aligned_alloc(64, RECOG_FRAME_BYTES,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
    if (!s_frame) {
        s_state = FACE_FAIL;
        ESP_LOGE(TAG, "frame buffer alloc FAILED (%u B PSRAM)", (unsigned)RECOG_FRAME_BYTES);
        dump_psram_blocks("frame buffer alloc failed");
        return;
    }
    if (!s_result) {
        s_result = (recog_result_t *)heap_caps_calloc(1, sizeof(recog_result_t), MALLOC_CAP_SPIRAM);
        if (!s_result) {
            s_state = FACE_FAIL;
            ESP_LOGE(TAG, "result snapshot alloc FAILED (%u B)", (unsigned)sizeof(recog_result_t));
            dump_psram_blocks("result snapshot alloc failed");
            engine_unload();
            return;
        }
    }

    /* ---- ② 把三个模型整份读进 PSRAM（★ 真正的 flash IO 全在这一步里，见文件头第四个坑）----
     * ★ 顺序：**先小后大**（MSR 211 KB → MNP 127 KB → MFN 1.29 MB），
     *   让最大那笔从仍然完整的大块里一次拿走（§14.3 实测：MFN 装卸一次会让
     *   PSRAM largest 从 4.25 MB 掉到 2.94 MB，能晚一点制造碎片就晚一点）。 */
    int64_t tr = esp_timer_get_time();
    if (!read_file_to_psram(RECOG_MSR_PATH, false, &s_msr_buf, &msr_b) ||
        !read_file_to_psram(RECOG_MNP_PATH, false, &s_mnp_buf, &mnp_b) ||
        !read_file_to_psram(RECOG_MFN_PATH, false, &s_mfn_buf, &mfn_b)) {
        s_state = FACE_FAIL;
        ESP_LOGE(TAG, "model read FAILED (see above) | PSRAM free=%uKB largest=%uKB",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
        dump_psram_blocks("model read failed");
        engine_unload();
        return;
    }
    uint32_t read_ms = (uint32_t)((esp_timer_get_time() - tr) / 1000);

    /* ---- ③ MSR + MNP（5 点关键点）---- */
    int64_t t1 = esp_timer_get_time();
    s_det = new DetBuf();
    s_det->msr = new MsrBuf(s_msr_buf);
    if (!s_det->msr->ok_) {
        s_state = FACE_FAIL;
        ESP_LOGE(TAG, "MSR construct FAILED");
        dump_psram_blocks("MSR construct failed");
        engine_unload();
        return;
    }
    s_det->mnp = new MnpBuf(s_mnp_buf);
    uint32_t msr_ms = (uint32_t)((esp_timer_get_time() - t1) / 1000);
    if (!s_det->mnp->ok_) {
        s_state = FACE_FAIL;
        ESP_LOGE(TAG, "MNP construct FAILED");
        dump_psram_blocks("MNP construct failed");
        engine_unload();
        return;
    }

    /* ---- ④ MFN（特征）---- */
    int64_t t2 = esp_timer_get_time();
    s_mfn = new MFN(s_mfn_buf);
    uint32_t mfn_ms = (uint32_t)((esp_timer_get_time() - t2) / 1000);
    if (!s_mfn || !s_mfn->ok()) {
        s_state = FACE_FAIL;
        ESP_LOGE(TAG, "MFN load FAILED after %u ms", (unsigned)mfn_ms);
        ESP_LOGE(TAG, "  -> 日志里是 'Failed to alloc' 就是内存；若是 fbs 解析错则是模型不兼容本芯片");
        /* ★★ 这一 dump 是 §67 排查里最缺的东西：esp-dl 只会给一个"largest available
         *   block size"数字，而"哪几块、各多大、能否合并"决定了下一步该省谁。
         *   注意此处**MSRMNP 已经装上了** ⇒ 分布里能看到"MSR/MNP 把大块切成了什么样"，
         *   这正是 P0-A（MFN 换 flash 分区）能不能救回来的直接依据。 */
        dump_psram_blocks("MFN load failed (MSRMNP already resident)");
        engine_unload();
        return;
    }

    s_state = FACE_RUNNING;
    /* ★ 收尾现场：与上面 "recogmem pre" 配对 ⇒ 两者之差 = 本引擎的**真实常驻代价**。
     *   INT 那一栏尤其关键：`ModelContext::root_alloc()` 的 m_internal_root
     *   走 MALLOC_CAP_INTERNAL（内部堆），而本工程内部堆余量只有几十 KB。 */
    {
        char who[96];
        snprintf(who, sizeof(who), "took=%lldms read=%ums msrmnp=%ums mfn=%ums",
                 (long long)((esp_timer_get_time() - t0) / 1000),
                 (unsigned)read_ms, (unsigned)msr_ms, (unsigned)mfn_ms);
        mem_diag_snap(TAG, "recogmem post", who);
    }
    ESP_LOGI(TAG, "recog engine ready in %lld ms (read %u + msrmnp %u + mfn %u) | "
                  "feat_dim=%d | PSRAM free=%uKB largest=%uKB",
             (long long)((esp_timer_get_time() - t0) / 1000),
             (unsigned)read_ms, (unsigned)msr_ms, (unsigned)mfn_ms,
             s_mfn->get_feat_len(),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
}

/** RGB565LE → RGB888（就地写进 s_frame）。与 face_service.cpp 的转换逐字一致。 */
void frame_to_rgb888(const uint16_t *src, int n)
{
    uint8_t *dst = s_frame;
    for (int i = 0; i < n; i++) {
        uint16_t p = src[i];
        dst[0] = (uint8_t)(((p >> 11) & 0x1F) << 3);
        dst[1] = (uint8_t)(((p >> 5) & 0x3F) << 2);
        dst[2] = (uint8_t)((p & 0x1F) << 3);
        dst += 3;
    }
}

/**
 * ★★ 关键点几何自检 —— 把方案 §8-S8-0 那条一直挂着的验证项
 *    "MSR+MNP 能否稳定输出 5 点关键点"变成**一次烧录就能读数**的形式。
 *
 * 为什么必须做：MSR/MNP 这一侧**没有任何"产物判据"**（见文件头第三条坑）——
 * MSRMNP 构造只要内存够就不会报错，`run()` 也一定返回一个 list。
 * 也就是说，"模型跑起来了"和"5 点是对的"在日志里长得**一模一样**。
 * 而 5 点一旦是垃圾，仿射对齐会把脸扭成一个谁也认不出的形状，
 * 表现却是"识别率莫名很低"，根本追不回来。
 *
 * 两个判据**刻意避开"哪边是左"**（点序陷阱让我们不能假设 x 的大小关系）：
 *   ① 5 点都落在人脸框附近（半框容差）—— 抓"全 0 / 巨偏"这类崩坏；
 *   ② 垂直关系：两只眼都在**同侧**嘴角上方，鼻尖在两眼的 y 与两嘴角的 y 之间
 *      —— 抓"点序被重排"与"上下颠倒的人脸"。
 *
 * @return 0 = 正常；>0 = 失败编码（1 出框 / 2 左眼低于左嘴角 / 3 右眼低于右嘴角 /
 *         4 鼻尖不在眼下方 / 5 鼻尖不在嘴角上方）。编码写进日志，一眼看出坏在哪条。
 *
 * ★ 容差给半框而不是"严格在框内"：MNP 的 box 与 landmark 是**两路独立回归**
 *   （见 dl_detect_mnp_postprocessor.cpp），landmark 略出框是正常的。
 *   判据宁松勿紧 —— 假警报会把真正的异常淹掉。
 */
static int keypoint_check(const int *kp, const int *box)
{
    const int x0 = box[0], y0 = box[1], x1 = box[2], y1 = box[3];
    const int mx = (x1 - x0) / 2;
    const int my = (y1 - y0) / 2;

    for (int i = 0; i < RECOG_KP_NUM; i++) {
        int px = kp[i * 2], py = kp[i * 2 + 1];
        if (px < x0 - mx || px > x1 + mx || py < y0 - my || py > y1 + my) return 1;
    }

    /* ★ 下标 ← 点序：kp[0..9] = [左眼x,左眼y, 左嘴角x,左嘴角y, 鼻尖x,鼻尖y,
     *                              右眼x,右眼y, 右嘴角x,右嘴角y]
     *   与 dl_feat_image_preprocessor.cpp 的 s_std_ldks_112 同序（文件头"点序陷阱"）。 */
    const int eye_l_y = kp[1], mth_l_y = kp[3], nose_y = kp[5], eye_r_y = kp[7], mth_r_y = kp[9];
    if (eye_l_y >= mth_l_y) return 2;
    if (eye_r_y >= mth_r_y) return 3;
    const int eye_max = (eye_l_y > eye_r_y) ? eye_l_y : eye_r_y;
    const int mth_min = (mth_l_y < mth_r_y) ? mth_l_y : mth_r_y;
    if (nose_y <= eye_max) return 4;
    if (nose_y >= mth_min) return 5;
    return 0;
}

/**
 * 识别本体（**只在一次性任务里跑**）。
 *
 * 流程严格按方案 §5.3：检出（MSR+MNP，自带 5 点）→ 对齐（FeatImagePreprocessor
 * 内部做仿射）→ 提特征（MFN）→ 查库（face_db_find_best）。
 */
void recog_run_once(recog_job_t job)
{
    int64_t t0 = esp_timer_get_time();
    recog_result_t *r = s_result;
    memset(r, 0, sizeof(*r));
    r->person_id = FACE_DB_GUEST_ID;

    dl::image::img_t img = {};
    img.data     = s_frame;
    img.width    = s_frame_w;
    img.height   = s_frame_h;
    img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888;

    /* ---- ① 检出 + 关键点（MSR 粗检出 → MNP 精修出 5 点）----
     * ★ 选**面积最大**的那张脸：与 CAM 页预览画框的选法一致（所见即所识）。
     * ★ 不能选"分数最高"：MSR 的分数对小脸普遍偏高，会挑到背景里那张。 */
    auto &res = s_det->run(img);
    const dl::detect::result_t *best = nullptr;
    int best_area = 0;
    for (auto &d : res) {
        if (d.box.size() < 4) continue;
        if (d.keypoint.size() < RECOG_KP_NUM * 2) continue;   /* 没有 5 点就没法对齐 */
        int a = (d.box[2] - d.box[0]) * (d.box[3] - d.box[1]);
        if (a > best_area) { best_area = a; best = &d; }
    }
    if (best == nullptr) {
        r->valid = false;
        s_last_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        ESP_LOGD(TAG, "no face with keypoints (%u ms)", (unsigned)s_last_ms);
        return;
    }

    r->x = best->box[0];
    r->y = best->box[1];
    r->w = best->box[2] - best->box[0];
    r->h = best->box[3] - best->box[1];
    r->det_score = best->score;             /* ★ 检出置信度（质量门槛用它，别拿相似度当它） */
    int kpc = 0;                            /* 关键点自检码；0 = 正常，作用域要到函数末尾的日志 */
    for (int i = 0; i < RECOG_KP_NUM * 2; i++) {
        r->kp[i] = best->keypoint[i];
    }

    /* ---- ★ 关键点自检（方案 §8-S8-0 那个"5 点是否稳定"的验证项就在这里落地）----
     * ★ 正常时**不刷屏**（USER 态每 1.5 s 一次）；异常必报，且累计计数给在任务收尾日志里。
     * ★ EXTRACT（录入）时每张都打全量 5 点：一次录入只有 3 行，正好当验收证据。 */
    {
        kpc = keypoint_check(r->kp, best->box.data());
        s_runs++;
        if (kpc != 0) {
            s_kp_bad++;
            ESP_LOGW(TAG, "keypoint ANOMALY code=%d | kp=[%d,%d %d,%d %d,%d %d,%d %d,%d] | box=%d,%d %dx%d | bad=%u/%u",
                     kpc,
                     r->kp[0], r->kp[1], r->kp[2], r->kp[3], r->kp[4],
                     r->kp[5], r->kp[6], r->kp[7], r->kp[8], r->kp[9],
                     r->x, r->y, r->w, r->h,
                     (unsigned)s_kp_bad, (unsigned)s_runs);
        } else if (job == RECOG_JOB_EXTRACT) {
            ESP_LOGI(TAG, "keypoints ok | kp=[%d,%d %d,%d %d,%d %d,%d %d,%d] | box=%d,%d %dx%d",
                     r->kp[0], r->kp[1], r->kp[2], r->kp[3], r->kp[4],
                     r->kp[5], r->kp[6], r->kp[7], r->kp[8], r->kp[9],
                     r->x, r->y, r->w, r->h);
        }
    }

    /* ---- ② 提特征 ----
     * ★ 关键点**原样透传**（顺序见文件头"点序陷阱"，重排会静默降低准确率）。 */
    dl::TensorBase *feat = s_mfn->run(img, best->keypoint);
    if (feat == nullptr) {
        r->valid = false;
        s_last_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        ESP_LOGW(TAG, "MFN returned null (%u ms)", (unsigned)s_last_ms);
        return;
    }
    {
        int nb = feat->get_dtype_bytes();
        int n  = feat->get_size();
        if (nb < 4 || n <= 0) {
            r->valid = false;
            s_last_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
            ESP_LOGE(TAG, "unexpected feat tensor: dtype_bytes=%d size=%d", nb, n);
            return;
        }
        const float *f = (const float *)feat->get_element_ptr();
        if (f == nullptr) {
            r->valid = false;
            s_last_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
            ESP_LOGE(TAG, "feat element ptr is null");
            return;
        }
        int m = (n < RECOG_FEAT_DIM) ? n : RECOG_FEAT_DIM;
        for (int i = 0; i < m; i++) r->feat[i] = f[i];
        if (n != RECOG_FEAT_DIM) {
            /* 不是致命错误（库维度以 face_db 记的为准），但要说出来 ——
             * 换模型后维度变了却没人改 RECOG_FEAT_DIM 就是这种症状。 */
            ESP_LOGW(TAG, "feat_len=%d != RECOG_FEAT_DIM(%d) - model swapped?", n, RECOG_FEAT_DIM);
        }
    }

    r->valid = true;

    /* ---- ③ 查库（EXTRACT 作业不做这一步）----
     * ★ §73.22：未命中时把"库内最高分者是谁"带进日志 —— 2026-09-30 打卡排查
     *   的实测盲区：只有 score=0.556 而不知道跟谁比的，"底片质量差"与
     *   "库里是别人/脏数据"无法区分。face_db_find_best 现在未命中也回填
     *   m.person（见 face_db.c），这里顺手接住。 */
    const char *best_name = "-";    /* 未识别/EXTRACT 时无意义，打 '-' */
    if (job == RECOG_JOB_IDENTIFY) {
        face_db_match_t m;
        memset(&m, 0, sizeof(m));
        int slot = face_db_find_best(r->feat, RECOG_FEAT_DIM, RECOG_MATCH_THR, &m);
        r->score = m.score;
        if (slot >= 0 && m.matched) {
            r->matched   = true;
            r->person_id = m.person.id;
            memcpy(r->name, m.person.name, sizeof(r->name));
            r->name[sizeof(r->name) - 1] = '\0';
        } else {
            /* 未命中：落到访客（R3 不误报 —— 阈值没过就当陌生人，不硬套一个名字）。
             * score 仍然给出来（是"最高分"），UI 上显示它便于用户判断阈值松紧。
             * ★ m.person 已回填库内最高分者（slot>=0 时），仅供日志诊断。 */
            r->matched   = false;
            r->person_id = FACE_DB_GUEST_ID;
            r->name[0]   = '\0';
            if (slot >= 0 && m.person.name[0] != '\0') {
                best_name = m.person.name;
            }
        }
    } else {
        r->score = 0.0f;
    }

    s_last_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    ESP_LOGI(TAG, "%s: %s%s (score=%.3f, best=%s, db=%d, box=%d,%d %dx%d, kp=%s, %u ms)",
             (job == RECOG_JOB_IDENTIFY) ? "identify" : "extract",
             r->matched ? "MATCH " : "guest ", r->name,
             r->score, best_name, face_db_count(), r->x, r->y, r->w, r->h,
             (kpc == 0) ? "ok" : "ANOMALY",
             (unsigned)s_last_ms);
}

/** 一次性识别任务：跑完发布结果并自删。 */
void recog_task(void *arg)
{
    (void)arg;
    recog_job_t job = s_job_is_extract ? RECOG_JOB_EXTRACT : RECOG_JOB_IDENTIFY;

    recog_run_once(job);

    /* ★ 顺序不能反：先把结果写完，最后再清 running ——
     *   `face_recog_busy()` 看的是 (running || result_new)，清 running 之后
     *   结果一定已经可见（单写者 + volatile，够用；不为这点事引入锁）。 */
    s_result_new  = true;
    s_job_running = false;

    /* ★ 自报栈水位：这是**唯一**能告诉我们 8192 够不够的数据。
     *   必须是最后几行之一 —— 之后不能再碰任何栈上变量。 */
    ESP_LOGI(TAG, "recog task stack hwm=%u B (size=%d) - tune RECOG_TASK_STACK with this",
             (unsigned)uxTaskGetStackHighWaterMark(NULL), RECOG_TASK_STACK);
    ESP_LOGI(TAG, "recog stats: runs=%u | keypoint ANOMALY=%u (%.1f%%)",
             (unsigned)s_runs, (unsigned)s_kp_bad,
             s_runs ? (100.0 * (double)s_kp_bad / (double)s_runs) : 0.0);

    vTaskDeleteWithCaps(NULL);        /* PSRAM 栈必须用 WithCaps 回收 */
}

} // namespace

/* ---------------------------------------------------------------------------
 *  ★★ 一次性加载任务 —— "谁来执行 engine_load()" 的唯一答案
 *
 *  ★★★ 2026-09-28 上机事故（P0）：`face_recog_poll()` 是 `s_load_pending` 的
 *      **唯一消费者**，而 UI 侧一次都没调它 ⇒
 *        `recog ON, engine load requested`（set_enabled 打的）
 *        → 之后**既没有** `starting recog load task...`，**也没有** `recog engine ready`
 *        → `face_recog_submit()` 每次都因 `!face_recog_ready()` 返回 ESP_ERR_INVALID_STATE
 *          （而且**静默**，一行日志都没有）
 *        → ENROLL 永远停在 `capturing 0/3`，表现成"按了没反应"。
 *      同时看门狗挂在 `face_recog_state()` 里、UI 也不调它 ⇒ 超时诊断同样永远不报。
 *
 *  修法两条（缺一不可）：
 *    ① 加载改成**一次性任务**，`face_recog_poll()` 只负责起任务（非阻塞）⇒
 *       于是 poll 可以在 LVGL 线程安全周期调用，不再受"cam_task 必须在跑"的约束；
 *    ② 看门狗搬进 `face_recog_poll()` ⇒ "请求了 8 s 还没开始加载"必然留痕。
 * ------------------------------------------------------------------------- */
static void recog_load_task(void *arg)
{
    (void)arg;
    engine_load();

    /* ★★ 加载期间有人按了关（`face_recog_stop()` 会被拒，但它**已经把 s_enabled
     *   清成 false**）⇒ 由本任务补做卸载。
     *   不补的话：宿主每次 pump 重试能兜住 —— **但 CAM 页一销毁就没人重试了**
     *   （`tab_camera_destroy` 删掉定时器），1.29 MB 的 MFN 会**永远常驻** PSRAM。
     *   这里复用 s_enabled，不引入新标志位。 */
    if (!s_enabled) {
        engine_unload();
        s_state = FACE_IDLE;
        ESP_LOGI(TAG, "stopped while loading -> engine released immediately");
    }

    /* ★ 自报栈水位：这是唯一能证明 10240 够不够的数据（首次上机后按它调）。
     *   之后不能再碰任何栈上变量之外的东西。 */
    ESP_LOGI(TAG, "recog load task stack hwm=%u B (size=%d) - tune RECOG_LOAD_STACK with this",
             (unsigned)uxTaskGetStackHighWaterMark(NULL), RECOG_LOAD_STACK);
    s_load_running = false;           /* ★ 先清 running 再自删：stop() 看它决定能不能卸载 */
    vTaskDeleteWithCaps(NULL);        /* PSRAM 栈必须用 WithCaps 回收 */
}

/* ---------------------------------------------------------------------------
 *  对外 API（S8-2）
 * ------------------------------------------------------------------------- */
esp_err_t face_recog_init(void)
{
    s_state         = FACE_IDLE;
    s_load_pending  = false;
    s_enabled       = false;
    s_job_running   = false;
    s_load_running  = false;
    s_stop_defer_logged = false;
    s_result_new    = false;
    s_job_is_extract = false;
    s_last_ms       = 0;
    ESP_LOGI(TAG, "init (engine resident, models load on demand)");
    return ESP_OK;
}

void face_recog_request_load(void)
{
    (void)face_recog_set_enabled(true);
}

void face_recog_poll(void)
{
    /* ---- ① 看门狗（★ 从 face_recog_state() 搬过来的，理由见上面的事故记录）----
     * 「请求了 8 s 却**连加载任务都没起来**」只可能是 poll 没被周期调用 ——
     * 这正是本次事故的形态，以后必然可诊断。 */
    if (s_state == FACE_LOADING && !s_load_running &&
        esp_timer_get_time() - s_load_req_us > RECOG_LOAD_TIMEOUT_US) {
        s_load_pending = false;
        s_state        = FACE_FAIL;
        ESP_LOGE(TAG, "recog load never started within %d ms - face_recog_poll() not called?",
                 (int)(RECOG_LOAD_TIMEOUT_US / 1000));
    }

    if (!s_load_pending) return;
    if (face_recog_ready()) { s_load_pending = false; return; }
    if (s_load_running) return;                       /* 已在加载：别重复起任务 */
    if (!s_enabled) {                                 /* 请求已被撤销（stop 抢先） */
        s_load_pending = false;
        s_state        = FACE_IDLE;
        return;
    }

    s_load_pending = false;
    s_load_running = true;
    ESP_LOGI(TAG, "starting recog load task (MSR+MNP+MFN, stack=%d, PSRAM)...", RECOG_LOAD_STACK);
    if (xTaskCreatePinnedToCoreWithCaps(recog_load_task, "recog_load", RECOG_LOAD_STACK, nullptr,
                                        RECOG_TASK_PRIO, nullptr, RECOG_TASK_CORE,
                                        RECOG_TASK_MEM_CAPS) != pdPASS) {
        /* ★ PSRAM 栈也建不起来才报错（内部 RAM 只剩 TCB 的那几十字节需求）。 */
        s_load_running = false;
        s_state        = FACE_FAIL;
        ESP_LOGE(TAG, "xTaskCreate(recog_load) FAILED (PSRAM tight, stack=%d)",
                 RECOG_LOAD_STACK);
    }
}

esp_err_t face_recog_stop(void)
{
    s_enabled      = false;
    s_load_pending = false;
    /* ★★ 两种"不能卸载"的情形，都造成确定的 UAF / 重复 free：
     *   ① 识别任务在跑 —— 它读的是 s_frame；
     *   ② **加载任务在跑** —— 它正在写 s_det / s_mfn / s_frame，
     *      此刻 engine_unload() 就是把它脚下的指针删掉（2026-09-28 补的；
     *      旧代码只看 s_job_running，裸奔）。
     *   ⇒ 拒绝并保持"加载中/运行中"，让宿主下一拍重试
     *     （recog_pump 已经是"被拒就把期望值留 true"的写法）。 */
    if (s_job_running || s_load_running) {
        if (!s_stop_defer_logged) {
            s_stop_defer_logged = true;
            ESP_LOGW(TAG, "stop deferred: %s still running (host will retry next tick)",
                     s_load_running ? "the load task" : "a recognition job");
        }
        s_state = s_load_running ? FACE_LOADING : FACE_RUNNING;
        return ESP_ERR_INVALID_STATE;
    }
    s_stop_defer_logged = false;
    engine_unload();
    s_state = FACE_IDLE;
    ESP_LOGI(TAG, "recog engine released");
    return ESP_OK;
}

face_state_t face_recog_state(void)
{
    /* ★ 纯 getter（看门狗已搬进 face_recog_poll()）——
     *   旧版把超时判定放在这里，而 UI 从不调用本函数 ⇒ 超时永远不报。
     *   教训：**副作用不要藏在 getter 里**，除非调用点被保证存在。 */
    return s_state;
}

bool face_recog_ready(void)
{
    return s_state == FACE_RUNNING && s_det != nullptr && s_mfn != nullptr && s_frame != nullptr;
}

esp_err_t face_recog_set_enabled(bool on)
{
    if (!on) return face_recog_stop();
    s_enabled = true;
    if (!face_recog_ready()) {
        if (s_state != FACE_LOADING) {
            s_state = FACE_LOADING;
            s_load_req_us = esp_timer_get_time();
        }
        s_load_pending = true;
        ESP_LOGI(TAG, "recog ON, engine load requested");
    } else {
        ESP_LOGI(TAG, "recog ON (engine already resident)");
    }
    return ESP_OK;
}

bool face_recog_enabled(void)
{
    return s_enabled;
}

bool face_recog_busy(void)
{
    return s_job_running || s_result_new;
}

/* ===========================================================================
 *  ★★ §73.21 会话独占（claim）—— 实测事故的根治
 *
 *  事故（2026-09-30）：MCP 打卡会话（ai_punch）接管相机期间，CAM 页的
 *  recog_pump 仍在跑 —— 它的守卫 `camera_service_is_running()`（tab_camera
 *  ④）把"我们起的流"当成了"自己的预览在跑"，于是用**停流前的陈旧 canvas
 *  帧**连续提交识别作业，并把我们会话的结果抢走消费（实锤日志：
 *  `TAB_CAM: punch: NO MATCH (guest, det=0.97)`）。两个驱动 + 单槽结果总线
 *  ⇒ 我们读到的是"路过时的陈旧帧"的 guest 结论 ⇒ 误判"人脸库里没有你"。
 *
 *  修法：引擎级认领——认领后其他任务的 submit / result / consume 一律被拒，
 *  引擎与结果通道由认领任务独占。识别周期按"认领任务句柄"判定，不用加锁
 *  （认领/释放/提交/取结果都在同一任务内完成；TAB_CAM 在 LVGL 任务、
 *  ai_punch 在 WS 收包任务，天然互斥）。
 * =========================================================================== */
static TaskHandle_t s_claim_task = nullptr;

static bool session_blocked(void)
{
    return s_claim_task != nullptr && xTaskGetCurrentTaskHandle() != s_claim_task;
}

bool face_recog_session_claim(void)
{
    TaskHandle_t cur = xTaskGetCurrentTaskHandle();
    if (s_claim_task == cur) {
        return true;    /* 重复认领：幂等 */
    }
    if (s_claim_task != nullptr) {
        ESP_LOGW(TAG, "session claim refused (held by %s)", pcTaskGetName(s_claim_task));
        return false;
    }
    s_claim_task = cur;
    ESP_LOGI(TAG, "session claimed by %s (others' submit/result blocked)", pcTaskGetName(cur));
    return true;
}

void face_recog_session_release(void)
{
    if (s_claim_task != nullptr && s_claim_task == xTaskGetCurrentTaskHandle()) {
        s_claim_task = nullptr;
        ESP_LOGI(TAG, "session released");
    }
}

bool face_recog_session_is_claimed(void)
{
    return s_claim_task != nullptr;
}

esp_err_t face_recog_submit(const uint16_t *rgb565le, uint16_t w, uint16_t h, recog_job_t job)
{
    if (session_blocked()) {
        /* ★ §73.21：被认领任务独占期间，其他调用方一律拒绝（含退避提示）。
         *   tab_camera ④ 对 INVALID_STATE 已有"留话 + 500ms 退避"的处理。 */
        return ESP_ERR_INVALID_STATE;
    }
    if (rgb565le == nullptr) return ESP_ERR_INVALID_ARG;
    if (w == 0 || h == 0 || w > RECOG_MAX_W || h > RECOG_MAX_H) {
        /* ★ 说清楚而不是静默丢弃：这个限制来自帧缓冲的编译期大小。 */
        ESP_LOGE(TAG, "frame %ux%u not supported (max %dx%d)", w, h, RECOG_MAX_W, RECOG_MAX_H);
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_enabled || !face_recog_ready()) return ESP_ERR_INVALID_STATE;
    if (face_recog_busy())       return ESP_ERR_INVALID_STATE;

    /* ★★ 同步转换：调用方（LVGL 线程）返回后可以立刻改写自己的画布 ——
     *    这是"不跨线程共享帧指针"的实现方式，也顺手躲开了 §3-约束③的悬垂指针坑。 */
    frame_to_rgb888(rgb565le, (int)w * (int)h);
    s_frame_w = (int)w;
    s_frame_h = (int)h;
    s_job_is_extract = (job == RECOG_JOB_EXTRACT);

    s_job_running = true;

    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(recog_task, "recog_run",
                                                    RECOG_TASK_STACK, nullptr,
                                                    RECOG_TASK_PRIO, nullptr, RECOG_TASK_CORE,
                                                    RECOG_TASK_MEM_CAPS);
    if (ok != pdPASS) {
        /* ★ 同一口径：栈走 PSRAM，只有 PSRAM 也凑不出这块才失败。 */
        s_job_running = false;
        ESP_LOGE(TAG, "xTaskCreate(recog_run) FAILED (PSRAM tight, stack=%d)",
                 RECOG_TASK_STACK);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

const recog_result_t *face_recog_result(void)
{
    if (session_blocked()) {
        return nullptr;   /* §73.21：结果通道归认领任务，其他人看不见 */
    }
    if (!s_result_new || s_result == nullptr) return nullptr;
    return s_result;
}

void face_recog_result_consume(void)
{
    if (session_blocked()) {
        return;           /* §73.21：同上——不许替认领任务消费结果 */
    }
    s_result_new = false;
}

uint32_t face_recog_last_ms(void)
{
    return s_last_ms;
}

const uint8_t *face_recog_frame(int *w, int *h)
{
    if (s_frame == nullptr) return nullptr;
    if (w) *w = s_frame_w;
    if (h) *h = s_frame_h;
    return s_frame;
}

/* ---------------------------------------------------------------------------
 *  S8-0 自检
 * ------------------------------------------------------------------------- */
esp_err_t face_recog_probe(face_recog_probe_t *out)
{
    /* ★ 不能用 `= {0}`：C++ 的 -Wextra 会报 -Werror=missing-field-initializers
     *  （C 允许"部分初始化、其余归零"，C++ 认为"你漏写了成员"）。memset 最省事且零歧义。 */
    face_recog_probe_t local_buf;
    memset(&local_buf, 0, sizeof(local_buf));
    if (out == nullptr) {
        out = &local_buf;                /* 允许 NULL：此时只打日志 */
    }
    memset(out, 0, sizeof(*out));

    ESP_LOGI(TAG, "================ S8-0 MFN feasibility probe ================");
    ESP_LOGI(TAG, "model path: %s", RECOG_MFN_PATH);

    /* ---- ① 文件在位？--------------------------------------------------
     * ★ 先查文件再谈内存：路径写错和内存不够是两种完全不同的失败，
     *   混在一起报会让人往错误方向查（本工程 §60c 的 littlefs 事故就是这么误判的）。 */
    struct stat st;
    if (stat(RECOG_MFN_PATH, &st) != 0 || st.st_size <= 0) {
        out->err = ESP_ERR_NOT_FOUND;
        snprintf(out->note, sizeof(out->note), "model missing or empty: %s", RECOG_MFN_PATH);
        ESP_LOGE(TAG, "[FAIL-1/3] %s", out->note);
        ESP_LOGE(TAG, "  -> check: 是否烧了 storage.bin? 文件名是否一字不差?");
        return out->err;
    }
    out->model_bytes = (size_t)st.st_size;
    ESP_LOGI(TAG, "[OK-1/3] file present: %u bytes", (unsigned)out->model_bytes);

    /* ---- ② PSRAM 连续块预检（§61 的闸门）------------------------------ */
    size_t need    = out->model_bytes + RECOG_ALLOC_SLACK;
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t free_sz = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t int_lg  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "mem: need_contig=%uKB | PSRAM free=%uKB largest=%uKB | INT largest=%uKB",
             (unsigned)(need / 1024), (unsigned)(free_sz / 1024),
             (unsigned)(largest / 1024), (unsigned)(int_lg / 1024));

    if (largest < need) {
        out->err = ESP_ERR_NO_MEM;
        snprintf(out->note, sizeof(out->note),
                 "PSRAM largest=%uKB < need %uKB", (unsigned)(largest / 1024), (unsigned)(need / 1024));
        ESP_LOGE(TAG, "[FAIL-2/3] %s", out->note);
        ESP_LOGE(TAG, "  -> 这是碎片问题，不是容量问题：先看块分布再决定对策");
        /* ★ 2026-09-28：曾经是 heap_caps_dump()（持堆锁打上千行 ⇒ WDT 崩机），
         *   改用 mem_diag_dump_top（锁内只收集、锁外打印）。见 dump_psram_blocks 注释。 */
        mem_diag_dump_top(TAG, "fail-2/3 largest<need");
        return out->err;
    }
    ESP_LOGI(TAG, "[OK-2/3] contiguous block sufficient");

    /* ---- ③ 加载 + 推理 ------------------------------------------------ */
    /* ★ 与 engine_load() 同一纪律（文件头"第四个坑"）：模型必须从**PSRAM 缓冲**建，
     *   构造里不许有任何 flash 访问。本探针任务虽然是内部栈（stat 安全），
     *   但 MFN 的构造已改成只吃缓冲 ⇒ 这里同样先 read_file_to_psram() 再 new。 */
    uint8_t *mfn_buf  = nullptr;
    size_t   mfn_size = 0;
    int64_t t0 = esp_timer_get_time();
    if (!read_file_to_psram(RECOG_MFN_PATH, false, &mfn_buf, &mfn_size)) {
        out->err = ESP_FAIL;
        snprintf(out->note, sizeof(out->note), "read model to PSRAM failed: %s", RECOG_MFN_PATH);
        ESP_LOGE(TAG, "[FAIL-3/3] %s", out->note);
        return out->err;
    }
    ESP_LOGI(TAG, "model buffered: read %u B in %ums", (unsigned)mfn_size,
             (unsigned)((esp_timer_get_time() - t0) / 1000));

    MFN *mfn = new MFN(mfn_buf);
    out->load_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);

    if (mfn == nullptr || !mfn->ok()) {
        out->err = ESP_FAIL;
        snprintf(out->note, sizeof(out->note),
                 "MFN load failed after %ums (see esp-dl log above)", (unsigned)out->load_ms);
        ESP_LOGE(TAG, "[FAIL-3/3] %s", out->note);
        ESP_LOGE(TAG, "  -> 若日志里是 'Failed to alloc' 则是内存；若是 fbs 解析错则是模型不兼容本芯片");
        delete mfn;                  /* ★ 先删模型（它的权重指向 mfn_buf） */
        heap_caps_free(mfn_buf);
        out->psram_free_kb    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024;
        out->psram_largest_kb = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024;
        return out->err;
    }
    out->loaded = true;

    /* 取输出维度：**自己判空**，不用继承来的 get_feat_len()（它在 m_model 为空时会崩） */
    dl::Model     *raw = mfn->get_raw_model();
    dl::TensorBase *o  = (raw != nullptr) ? raw->get_output() : nullptr;
    if (o == nullptr) {
        out->err = ESP_FAIL;
        snprintf(out->note, sizeof(out->note), "model has no output tensor");
        ESP_LOGE(TAG, "%s", out->note);
        delete mfn;
        heap_caps_free(mfn_buf);
        return out->err;
    }
    out->feat_len = o->get_size();
    ESP_LOGI(TAG, "loaded in %ums | output: size=%d dtype=%s bytes=%d",
             (unsigned)out->load_ms, out->feat_len, o->get_dtype_string(), o->get_bytes());

    /* 造图 + 标准 5 点 */
    uint8_t *img_buf = (uint8_t *)heap_caps_malloc(RECOG_INPUT_W * RECOG_INPUT_H * 3, MALLOC_CAP_SPIRAM);
    if (img_buf == nullptr) {
        out->err = ESP_ERR_NO_MEM;
        snprintf(out->note, sizeof(out->note), "failed to alloc %d B for synthetic input",
                 RECOG_INPUT_W * RECOG_INPUT_H * 3);
        ESP_LOGE(TAG, "%s", out->note);
        delete mfn;
        heap_caps_free(mfn_buf);
        return out->err;
    }
    make_synthetic_face(img_buf, RECOG_INPUT_W, RECOG_INPUT_H);

    std::vector<int> ldmk(10);
    for (int i = 0; i < 10; i++) {
        ldmk[i] = iround(kLandmark112[i]);
    }

    dl::image::img_t img = {
        .data     = img_buf,
        .width    = RECOG_INPUT_W,
        .height   = RECOG_INPUT_H,
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888,
    };

    int64_t t1 = esp_timer_get_time();
    dl::TensorBase *feat = mfn->run(img, ldmk);
    out->infer_ms = (uint32_t)((esp_timer_get_time() - t1) / 1000);

    if (feat == nullptr) {
        out->err = ESP_FAIL;
        snprintf(out->note, sizeof(out->note), "inference returned null after %ums", (unsigned)out->infer_ms);
        ESP_LOGE(TAG, "%s", out->note);
    } else {
        int nb = feat->get_dtype_bytes();
        int n  = feat->get_size();
        if (nb >= 4 && n > 0) {
            const float *f = (const float *)feat->get_element_ptr();
            if (f != nullptr) {
                for (int i = 0; i < 4 && i < n; i++) {
                    out->feat_head[i] = f[i];
                }
                /* L2 模长：FeatPostprocessor 内部做了 l2_norm，正常应 ≈1.0。
                 * 它是比"数值非零"更强的判据：全 0 → 0，NaN 传播 → NaN。 */
                double acc = 0.0;
                for (int i = 0; i < n; i++) {
                    acc += (double)f[i] * (double)f[i];
                }
                out->feat_norm = (float)sqrt(acc);
            }
        }
        ESP_LOGI(TAG, "inference: %ums | feat[0..3] = %.6f %.6f %.6f %.6f | L2=%.6f",
                 (unsigned)out->infer_ms,
                 out->feat_head[0], out->feat_head[1], out->feat_head[2], out->feat_head[3],
                 out->feat_norm);
    }

    /* ---- 清理：连模型一起还回去，让这次自检**对后续功能零影响** -------- */
    heap_caps_free(img_buf);
    delete mfn;                      /* ~MFN → ~FeatImpl 依次 delete 三件套 */
    heap_caps_free(mfn_buf);         /* ★ 顺序不能反：权重直接指向这块缓冲 */

    out->psram_free_kb    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024;
    out->psram_largest_kb = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024;

    if (out->err != ESP_OK) {
        return out->err;
    }

    ESP_LOGI(TAG, "================ S8-0 PROBE PASS ================");
    ESP_LOGI(TAG, "  MFN(p4) loads & runs on this chip. feat_len=%d", out->feat_len);
    ESP_LOGI(TAG, "  load=%ums infer=%ums | after release: PSRAM free=%uKB largest=%uKB",
             (unsigned)out->load_ms, (unsigned)out->infer_ms,
             (unsigned)out->psram_free_kb, (unsigned)out->psram_largest_kb);
    ESP_LOGI(TAG, "=================================================");
    return ESP_OK;
}
