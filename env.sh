#!/usr/bin/env bash
# 加载 VitaSDK 环境变量。用法: source env.sh
# 自动定位项目根目录（本文件所在目录），VitaSDK 须安装在项目内 vitasdk/
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export VITASDK="$SCRIPT_DIR/vitasdk"
export PATH="$VITASDK/bin:$PATH"
echo "VITASDK=$VITASDK"
