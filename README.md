# PSVSend

面向 **PS Vita** 的 [LocalSend](https://localsend.org) 协议兼容客户端，可在同一局域网内与其他 LocalSend 设备（手机、PC）互相传输文件。

> 本项目是独立开发的自制软件，仅兼容 LocalSend 的开放协议（v2），与 LocalSend 品牌及其项目无任何关联。

## 当前状态

- [x] VitaSDK 交叉编译环境 + vita2d 渲染骨架 + VPK 打包（含 LiveArea 素材）
- [x] LocalSend v2 发送链路：UDP 组播发现 / register / prepare-upload / upload
- [x] 文件发送（PSV → 手机 / PC，HTTP 明文与 HTTPS 加密均支持）
- [x] TLS 加密传输（mbedTLS 3.6.5，含 mTLS 客户端身份）
- [ ] 文件接收（其他设备 → PSV，开发中）

## 构建

构建需要 VitaSDK 交叉编译工具链，安装到项目根目录下的 `vitasdk/`（`env.sh` 会自动定位该目录）。

### 1. 安装 VitaSDK（任选一种）

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

### 2. 安装依赖库

本项目依赖 libvita2d、freetype、libpng、libjpeg、zlib 等库，用工具链自带的 vdpm 包管理器安装（`tools/vdpm` 是 vdpm 的本地克隆）：

```bash
vdpm vita2d freetype libpng libjpeg zlib
```

### 3. 编译打包

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

## 安装

1. 将 `psvsend.vpk` 拷贝到已破解 PS Vita 的 `ux0:data/`
2. 使用 VitaShell 打开并安装
3. 桌面出现 PSVSend 气泡

## 使用

1. 打开 PSVSend，等待 Wi-Fi 联网（状态指示变绿，设备列表出现局域网内的其他 LocalSend 设备）
2. 选中目标设备 → 进入文件浏览（ux0 目录树，可多选）→ 确认发送
3. 对方接受后，传输页显示逐文件进度，可随时取消

### HTTPS / mTLS 说明

发送端按对方 announce 的 `protocol` 自动选择明文 HTTP 或 HTTPS。2026 版官方 LocalSend（Rust 内核）对无浏览器会话的发送方强制要求**客户端证书**（mTLS），不出示证书直接握手失败（`certificate_required`）。为此 PSVSend 随固件内置一张设备身份证书（自签 RSA-2048，见 `src/id_cert.inc` / `src/id_key.inc`），HTTPS 连接自动出示；同时按对方 announce 的指纹（证书 SHA-256）锁定服务端证书，不依赖 CA 链。接收端只校验设备证书本身有效，无需信任本客户端。

## 目录结构

**随仓库分发：**

| 路径 | 说明 |
|------|------|
| `src/` | 源码（含发送客户端与内嵌设备身份证书） |
| `sce_sys/` | LiveArea 素材（图标 / 背景 / 启动图） |
| `docs/localsend-protocol/` | LocalSend 协议参考文档 |

**本地安装（不随仓库分发）：**

| 路径 | 来源 | 用途 |
|------|------|------|
| `tools/` | 克隆自 [vitasdk/samples](https://github.com/vitasdk/samples) 与 [vitasdk/vdpm](https://github.com/vitasdk/vdpm) | 官方示例与包管理器（本地参考） |
| `vitasdk/` | 获取自 [vitasdk.org](https://vitasdk.org)（安装步骤见构建章节） | 交叉编译工具链 |

克隆仓库后本地只有"随仓库分发"的部分；按构建章节安装工具链和依赖库后，完整的本地目录布局如下：

```text
项目根目录/
├── CMakeLists.txt              # 随仓库分发
├── README.md                   # 随仓库分发
├── LICENSE                     # 随仓库分发
├── .gitignore                  # 随仓库分发
├── env.sh                      # 随仓库分发
├── build.sh                    # 随仓库分发：一键构建脚本
├── src/                        # 随仓库分发：源码
│   ├── main.c                  # 入口
│   ├── api.c                   # 前后端契约：启动 / 网络巡检 / 设备快照
│   ├── net.c                   # SceNet 网络初始化
│   ├── discovery.c             # UDP 组播 announce / 监听 + 设备表
│   ├── http.c                  # HTTP 服务器：处理对方 register / info（设备入表）
│   ├── transfer.c              # 发送客户端：prepare-upload / upload（HTTP + HTTPS）
│   ├── identity.c / id_cert.inc / id_key.inc  # 内嵌设备身份证书（HTTPS mTLS）
│   ├── config.c / json_util.c / dlog.c
│   └── ui/                     # vita2d 界面（设备列表 / 文件浏览 / 进度）
├── sce_sys/                    # 随仓库分发：LiveArea 素材
│   ├── icon0.png
│   └── livearea/contents/
├── docs/                       # 随仓库分发：协议参考文档
│   └── localsend-protocol/
├── tools/                      # 本地克隆：官方示例与 vdpm
│   ├── samples/
│   └── vdpm/
└── vitasdk/                    # 本地安装：工具链 + 依赖库
    ├── bin/
    ├── arm-vita-eabi/
    └── lib/
```

## 许可

[Apache License 2.0](LICENSE)

图标为原创设计，与 LocalSend 品牌无关联。
