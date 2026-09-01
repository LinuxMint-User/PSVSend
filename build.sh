#!/usr/bin/env bash
# 一键构建脚本：配置 + 编译 + 打包 VPK
# 用法: ./build.sh
set -e

# 定位项目根目录（本脚本所在目录）
cd "$(dirname "${BASH_SOURCE[0]}")"

# 加载 VITASDK 环境变量（需已安装工具链到项目内 vitasdk/）
source ./env.sh

# 配置（交叉编译）
cmake -S . -B build

# 编译并打包
cmake --build build

echo "构建完成: build/psvsend.vpk"
