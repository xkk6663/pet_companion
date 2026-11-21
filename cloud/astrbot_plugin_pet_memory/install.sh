#!/usr/bin/env bash
# 把插件装进 AstrBot 数据目录；默认只拷贝，--restart 才重启容器。
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATA_DIR="${PET_ASTRBOT_DATA:-/opt/astrbot/data}"
DEST="${DATA_DIR}/plugins/astrbot_plugin_pet_memory"

install -d "${DEST}"
for f in main.py metadata.yaml _conf_schema.json README.md; do
  install -m 0644 "${HERE}/${f}" "${DEST}/${f}"
done
rm -rf "${DEST}/__pycache__"

echo "[install] 已拷贝到 ${DEST}"
echo "[install] 重启容器使其加载：docker restart astrbot"
echo "[install] 或先在 WebUI → 插件 → 重载"

if [[ "${1:-}" == "--restart" ]]; then
  docker restart astrbot
  echo "[install] astrbot 已重启"
fi
