# PSVSend

面向 **PS Vita** 的 [LocalSend](https://localsend.org) 协议兼容客户端，可在同一局域网内与其他 LocalSend 设备（手机、PC）互相传输文件。

> 本项目是独立开发的自制软件，仅兼容 LocalSend 的开放协议（v2），与 LocalSend 品牌及其项目无任何关联。

> 本项目与索尼（Sony）及 PlayStation 无任何关联或背书；需在自制系统（如 HENkaku / 变革）上运行，请自行了解并遵守所在地法律法规与平台条款，风险自负，仅建议用于个人合法用途。

> 当前版本 **v2.0.0**（LocalSend 协议 v2，适配官方客户端 v1.15+）；设置页底部「关于」可查看版本与适配信息。正式版 VPK 由 GitHub Actions 自动构建并发布到 [Releases](https://github.com/LinuxMint-User/PSVSend/releases)（草稿确认后公开），无需本地自行编译。

## 当前状态

- [x] VitaSDK 交叉编译环境 + vita2d 渲染骨架 + VPK 打包（含 LiveArea 素材）
- [x] LocalSend v2 发送链路：UDP 组播发现 / register / prepare-upload / upload
- [x] 文件发送（PSV → 手机 / PC，HTTP 明文与 HTTPS 加密均支持）
- [x] TLS 加密传输（mbedTLS 3.6.5，含 mTLS 客户端身份）
- [x] 文件接收（手机 / PC → PSV）：prepare-upload 弹确认 → upload 流式写盘，兼容无 Content-Length 的 chunked 上传
- [x] 主动扫描：Vita 收不了 UDP 组播，改向局域网 /24 各 IP 主动 HTTP 探测补全设备表
- [x] 网络稳定性：待机唤醒 / Wi-Fi 断开恢复后自动恢复发现（watch 看门狗 + 低频 netctl 轮询，v2.0.0）
- [x] 渲染稳定性：修复偶发 GPU render crash（界面撕裂后崩溃）——每帧等待 GPU 渲染完成（v2.0.0）
- [x] 中 / 英双语界面（设置页可切换）与设置页底部「关于」区（版本 + 适配 LocalSend 说明，v2.0.0）

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

本项目链接的库：mbedTLS、libvita2d、freetype、bzip2、libpng、libjpeg(-turbo)、zlib（清单见 `CMakeLists.txt` 的 `target_link_libraries`）。用 vdpm 包管理器安装（`tools/vdpm` 是 vdpm 的本地克隆）——**注意 vdpm 包名与库文件名不同**：vita2d 的包名是 `libvita2d`，jpeg 的包名是 `libjpeg-turbo`：

```bash
vdpm libvita2d mbedtls freetype bzip2 libpng libjpeg-turbo zlib
```

（官方预编译包已内置这些库；重复执行 vdpm 会检测到已装版本并跳过。）

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

> 不想本地构建？仓库已配置 GitHub Actions（[`.github/workflows/build-vpk.yml`](.github/workflows/build-vpk.yml)），在官方 vitasdk Docker 镜像内自动构建：推送 `v*` tag 或在 Actions 页手动触发，VPK 会上传为 **Releases 草稿**（人工确认后公开）。直接到 [Releases](https://github.com/LinuxMint-User/PSVSend/releases) 下载即可。

## 安装

> 开发与真机测试环境：**变革（HENkaku）3.65** 自制系统。其余固件 / 破解环境（3.60、3.68、3.73 等）未逐一验证，如遇问题欢迎提 issue 反馈（附固件与破解版本信息）。

1. 将 `psvsend.vpk` 拷贝到已破解 PS Vita 的 `ux0:data/`
2. 使用 VitaShell 打开并安装
3. 桌面出现 PSVSend 气泡

## 使用

### 发送

1. 打开 PSVSend，等待 Wi-Fi 联网（设备列表页不再显示未联网提示即已就绪，无独立状态灯）。对方在收到我们的 UDP announce 后会自动发 HTTP register，设备随即出现在列表；若对方没出现，按 **△** 手动扫描整个网段补全
2. 选中目标设备 → 进入文件浏览（ux0 目录树，可多选）→ 确认发送
3. 对方接受后，传输页显示逐文件进度，可随时取消

### 接收

1. 对方（LocalSend App / 桌面客户端）向我们发送文件时，PSV 自动弹出接收确认页，显示来者、平台、文件清单与总大小
2. 焦点默认在 **Accept**：确认后对方即可开传；**Setup** 进入接收设置，可逐文件取消勾选（未勾选的跳过，不出现在接受回执里）；**Reject** 拒绝（对方收到 403）
3. 确认后进入接收进度页，显示逐文件进度与总进度，可随时取消
4. 收到的文件存入 `ux0:data/psvsend/downloads/`，边收边写（`.part` 临时文件，收完改正式名）；与已有文件同名时自动在扩展名前加 `(k)` 序号，不覆盖。中途断流/校验失败会删除残 `.part`，下次启动也会清扫上次异常遗留的临时文件

### HTTPS / mTLS 说明

发送端按对方 announce 的 `protocol` 自动选择明文 HTTP 或 HTTPS。2026 版官方 LocalSend（Rust 内核）对无浏览器会话的发送方强制要求**客户端证书**（mTLS），不出示证书直接握手失败（`certificate_required`）。为此 PSVSend 随固件内置一张设备身份证书（自签 RSA-2048，见 `src/net/id_cert.inc` / `src/net/id_key.inc`），HTTPS 连接自动出示；同时按对方 announce 的指纹（证书 SHA-256）锁定服务端证书，不依赖 CA 链。接收端只校验设备证书本身有效，无需信任本客户端。

## 边界与已知限制

以下为当前实现的实际边界，均来自真机验证或代码常量。设计取舍与路线见 [docs/design.md](docs/design.md)。

### 网络 / 发现

- **只走 IPv4、协议 v2**，不实现 v3（签名 / WebRTC / PIN）与 IPv6
- **Vita 收不了 UDP 组播**：系统保留 53317（bind 报 EACCES），而 LocalSend 组播固定发往 `224.0.0.167:53317`，无法 bind 即无法收包。设备列表靠两条替代路径填充：对方主动 HTTP register（每 ~5s）+ 手动主动扫描（设备列表按 **△**）
- **主动扫描只覆盖 /24 子网**（假设掩码 255.255.255.0）：非常规子网（/16、/23 等）扫不全。扫描携带设备证书、逐 IP TLS→明文探测（2026 官方端默认 HTTPS，故 TLS 优先，明文兜底兼容纯 HTTP 端），对 HTTPS/mTLS 官方端有效（2026 版 Rust 内核要求出示客户端证书，见上）；**优先探测历史在线设备**（config `knownIps` 持久化，上限 24、LRU 淘汰），常用设备通常在轮次开始后 ~1s、进度极低时即出现；实测整轮约 6~10s（8 并发 worker），界面有进度；每轮会对未在线的 IP 全部空探一遍（无法跳过，扫描期间网络开销较低）
- **设备表 90s 无动静移除**：对方停止 announce/register 90s 后从列表消失
- **HTTP 服务器每连接独立 worker**：accept 线程只分发，一个连接一个处理线程（上限 8，满员立即关闭新连接）。忙时（本机正在收/发文件）对方的新请求**立即收到 409**（LocalSend 端显示"对方正在处理另一个请求"），不会在 TCP 队列里干等；空闲时 register/prepare 等请求不会被大文件 upload 阻塞。仍为**单活动接收会话**（一次只收一个设备，见下）

### 接收

- 多文件接收中途取消的竞态已修复（收满优先于取消判断）：正在传的文件若已收满则保留完整、会话以「用户取消」收尾，未收满才清理其残 `.part`；已收完的文件始终保留
- **接收端只监听明文 HTTP**（候选端口 4567/53318/…，announce 声明 `protocol:http`）；HTTPS 仅用于 PSV 作为发送方时。对方会按 announce 自动走明文
- **同一时刻只处理一个接收会话**：正在收/已结束未清场时，新的 prepare-upload 返回 409；**PSV 正在发送文件时同样 409**（发送中不收新文件），避免请求被晾到超时
- **清单上限**：单会话 ≤32 个文件（超出部分被忽略，回执只列前 32 个）；清单 JSON 体 ≤8KB；文件名 ≤192 字符截断
- **超时三档**：prepare 等界面决定 **60s**（超时回 403）；接受后/文件间空闲 **120s**（判定 TIMEOUT、清理临时文件）；收体中 socket 连续 **30s** 无数据判为断流
- **sha256 校验仅当对方提供**（prepare 清单里有 64 位 hex 才校验，不符回 422）；多数客户端默认带 sha256
- **保存目录固定** `ux0:data/psvsend/downloads/`（接收设置页尚不能选目录/改名）；同名文件自动在扩展名前加 `(k)`
- **单文件大小上限取决于文件系统**（ux0 为 FAT 系，单文件 ≤4GB-1）；磁盘写满时报错结束，传输中断

### 发送

- **文件浏览入口钉死 `ux0:/`**：目录树从 ux0 根出发，无法浏览外置卡 uma0/xmc0 等分区，只能发送 ux0 内的文件
- HTTPS 目标按 announce/扫描记录的证书指纹 pin：对方换证书（指纹随之变化）时需重新发现一次设备才会刷新指纹
- 发送大文件期间界面取消会中止当前文件与后续排队文件，已完成文件保留

### 界面显示

- **界面内嵌中文字体**（随 VPK 打包到 `app0:/fonts/`，源为 AOSP Droid Sans 与 Droid Sans Fallback Full，Apache-2.0）：ASCII/Latin 走 Droid Sans，CJK/全角走 Fallback，中英文正常显示；无字形的字符（emoji、个别生僻扩展区）直接不渲染，传输本身不受影响（渲染细节见 [docs/design.md](docs/design.md) §5.5/§9）

### 已知小问题

- 手动扫描对 HTTPS 官方端实测有效；对**纯 HTTP 明文端**的兼容只在逻辑上支持（明文 probe 兜底路径），开发环境未出现此类设备、尚未真机验证
- 改名相关交互未实装：文件浏览页与接收设置页的"改名"、设置页"主机名"目前均为占位弹窗（文字输入方案待定，见 [docs/design.md](docs/design.md) §9）

## 目录结构

**随仓库分发：**

| 路径 | 说明 |
|------|------|
| `src/` | 源码（含发送客户端与内嵌设备身份证书） |
| `sce_sys/` | LiveArea 素材（图标 / 背景 / 启动图） |
| `fonts/` | 界面内嵌字体：Droid Sans（拉丁）+ Droid Sans Fallback Full（CJK），均来自 AOSP（Apache-2.0），编译时打进 VPK `app0:/fonts/` |
| `docs/localsend-protocol/` | LocalSend 官方协议文档副本（互操作参考，来源与权利声明见 `ATTRIBUTION.md`） |
| `.github/workflows/` | GitHub Actions：自动构建 VPK 并发布 Releases 草稿 |

**本地安装（不随仓库分发）：**

| 路径 | 来源 | 用途 |
|------|------|------|
| `tools/` | 克隆自 [vitasdk/samples](https://github.com/vitasdk/samples) / [vitasdk/vdpm](https://github.com/vitasdk/vdpm) / [xyzz/vita-parse-core](https://github.com/xyzz/vita-parse-core) | 官方示例、vdpm 包管理器与 PSVita 核心转储解析工具（本地参考） |
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
├── .github/workflows/          # 随仓库分发：CI 构建 + Release 草稿发布
├── src/                        # 随仓库分发：源码（按依赖域分子目录，include 以 src/ 为根）
│   ├── main.c                  # 入口：启动后端 + UI
│   ├── app/                    # 装配层：api.c/h（前后端契约：启动 / 网络巡检 / 设备快照）
│   ├── core/                   # 基础设施：config（配置）/ dlog（日志）/ i18n（文案）/ json_util（JSON）
│   ├── net/                    # 网络与传输：net（初始化）/ discovery（发现+设备表）/ scan（主动扫描）
│   │                           #             http（HTTP 服务器+客户端）/ identity（TLS 设备身份，含 id_cert.inc / id_key.inc）
│   ├── proto/                  # LocalSend 协议会话：transfer（发送）/ receive（接收）
│   └── ui/                     # vita2d 界面（设备列表 / 文件浏览 / 传输 / 设置）
├── sce_sys/                    # 随仓库分发：LiveArea 素材
│   ├── icon0.png
│   └── livearea/contents/
├── docs/                       # 随仓库分发：设计文档与 LocalSend 协议副本
│   ├── design.md               # 架构 / 决策 / 路线
│   └── localsend-protocol/     # LocalSend 官方协议文档副本（来源/权利见其 ATTRIBUTION.md）
├── tools/                      # 本地克隆：官方示例 / vdpm / vita-parse-core
│   ├── samples/
│   ├── vdpm/
│   └── vita-parse-core/        # PSVita 核心转储解析工具（崩溃分析用）
└── vitasdk/                    # 本地安装：工具链 + 依赖库
    ├── bin/
    ├── arm-vita-eabi/
    └── lib/
```

## 许可

[Apache License 2.0](LICENSE)

图标为原创设计，与 LocalSend 品牌无关联。
