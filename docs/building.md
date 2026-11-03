# 构建指南

构建需要 VitaSDK 交叉编译工具链，安装到项目根目录下的 `vitasdk/`（`env.sh` 会自动定位该目录）。

## 1. 安装 VitaSDK（任选一种）

**方式 A：官方预编译包**

从 https://github.com/vitasdk/packages/releases 获取 Linux x86_64 的 nightly 包（`vitasdk-x86_64-linux-gnu-*.tar.bz2`），在项目根目录解压并改名为 `vitasdk`：

```bash
tar -xjf vitasdk-x86_64-*.tar.bz2
mv vitasdk-x86_64-* vitasdk
```

**方式 B：vdpm 源码构建**

```bash
git clone https://github.com/vitasdk/vdpm
cd vdpm && ./bootstrap-vitasdk.sh       # 默认安装到 ~/vitasdk
mv ~/vitasdk <项目根目录>/vitasdk
```

验证工具链就位：`ls vitasdk/bin/vita-mksfoex`

## 2. 安装依赖库

本项目链接的库：mbedTLS、libvita2d、freetype、bzip2、libpng、libjpeg(-turbo)、zlib（清单见 `CMakeLists.txt` 的 `target_link_libraries`）。二维码解码用随仓库入库的 quirc v1.2（BSD，`third_party/quirc/`），无需额外安装。用 vdpm 包管理器安装（`tools/vdpm` 是 vdpm 的本地克隆）——**注意 vdpm 包名与库文件名不同**：vita2d 的包名是 `libvita2d`，jpeg 的包名是 `libjpeg-turbo`：

```bash
vdpm libvita2d mbedtls freetype bzip2 libpng libjpeg-turbo zlib
```

（官方预编译包已内置这些库；重复执行 vdpm 会检测到已装版本并跳过。）

## 3. 编译打包

```bash
./build.sh          # 一键：配置 + 编译 + 打包
```

等价于手动执行：

```bash
source env.sh
cmake -S . -B build
cmake --build build
```

产物：`build/psvsend.vpk`

## 4. CI 构建

不想本地构建？仓库已配置 GitHub Actions（[`.github/workflows/build-vpk.yml`](../.github/workflows/build-vpk.yml)），在官方 vitasdk Docker 镜像内自动构建：推送 `v*` tag 或在 Actions 页手动触发，VPK 会上传为 **Releases 草稿**（人工确认后公开）。直接到 [Releases](https://github.com/LinuxMint-User/PSVSend/releases) 下载即可。
