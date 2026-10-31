#!/bin/bash
set -e
export DEBIAN_FRONTEND=noninteractive
echo "== apt install python3-venv =="
apt-get update -qq
apt-get install -y -qq python3.14-venv 2>&1 | tail -2
echo "== 清残留 venv 并重跑部署 =="
rm -rf /opt/pet_gateway/.venv
bash /tmp/gw/deploy_gateway.sh
