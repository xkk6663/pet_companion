/* static_pool.h - 编译期静态内部内存池（2026-11-01）
 *
 * ★ 解决的问题：本工程内部 RAM 的病不是"总量不够"，而是"最大的连续块不够"。
 *   典型现场（2026-11-01 打卡日志）：`INT free=7259 largest=7168` ——
 *   总量还有 7 KB，但连一个 10 KB / 4 KB 的栈都凑不出来。
 *
 * ★ 做法：在**编译期**（.bss）划出一片内部 RAM arena，只发给"**必须内部 RAM**、
 *   **尺寸固定**、**反复创建/销毁**"的对象（目前是两条短任务栈）：
 *   识别读数任务（4 KB，每次加载模型都建一次）与打卡落盘任务（4 KB，每次打卡都建）。
 *   ⇒ 这两类分配**永远不再碰内部堆**，也就永远不会被堆碎片卡住。
 *
 * ★ 三条纪律（照抄 face_recog_service.cpp 文件头"第四个坑"的口径）：
 *   ① 池里的内存**只增不减**（bump 分配，无回收）：没有 free 就没有"分配器本身
 *      把内存切碎"的可能，也没有释放/复用竞态（静态任务栈一旦复用给别的任务，
 *      正在退出的任务可能还在用那块栈 —— 这是 FreeRTOS 静态栈最隐蔽的坑）。
 *      代价是"池块终生绑定一个调用点"，所以池只服务固定尺寸的少数对象。
 *   ② 池**不是**通用 malloc 替代品：不要拿它装动态尺寸缓冲/模型/图像。
 *      图省那点碎片把大对象塞进来，只会把池撑爆并把真正需要保证的栈挤掉。
 *   ③ 池里全是内部 RAM：所以**不许**在 PSRAM 栈任务里用这些块做什么 flash IO
 *      之外的特殊事 —— 它和普通内部 RAM 一样，受 flash cache 关闭窗口影响。
 *
 * ★ 容量：STATIC_POOL_TOTAL 是**编译期常量**，改动等于直接改 .bss / DIRAM 占用。
 *   当前 12 KB（3 个 4 KB 块）：已用 8 KB（recog_read + punch），
 *   余 4 KB 留给同一类"关键时刻必须建得起来"的短任务栈。
 */

#pragma once

#include <stddef.h>

/* ★ 实现是 C（static_pool.c），调用方有 C++（face_recog_service.cpp）。
 *   不加这层包裹，C++ 会按名字修饰去找 `_Z17static_pool_allocjPKc`，
 *   而 .c 里导出的是 C 符号 ⇒ 链接期 undefined reference。 */
#ifdef __cplusplus
extern "C" {
#endif

/** 池总容量（字节，编译期）。对齐按 16 B，向下取整到 16 的倍数。 */
#define STATIC_POOL_TOTAL   (12 * 1024)

/**
 * @brief 从静态池切一块，16 B 对齐。
 *
 * ★ **终生不回收**：每个调用点应该在自己的 static 指针里缓存返回值，
 *   只在第一次（或 NULL 时）申请，之后反复创建任务都复用同一块。
 *   每次调用都申请会把池吃光（这是使用方最容易犯的错）。
 *
 * @param bytes 需要的字节数（内部按 16 B 向上取整）
 * @param who   调用点名字，只用于日志（池溢出时能一眼看出是谁要的）
 * @return 16 B 对齐的指针；池余量不足返回 NULL（并打印 need/used/total）
 */
void *static_pool_alloc(size_t bytes, const char *who);

/** 已用字节数（含对齐开销） */
size_t static_pool_used(void);

/** 剩余字节数 */
size_t static_pool_free(void);

#ifdef __cplusplus
}
#endif
