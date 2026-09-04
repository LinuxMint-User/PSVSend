# PSVSend

面向 **PS Vita** 的 [LocalSend](https://localsend.org) 协议兼容客户端，可在同一局域网内与其他 LocalSend 设备（手机、PC）互相传输文件。

> 本项目是独立开发的自制软件，仅兼容 LocalSend 的开放协议（v2），与 LocalSend 品牌及其项目无任何关联。

## 当前状态

- [x] VitaSDK 交叉编译环境 + vita2d 渲染骨架 + VPK 打包（含 LiveArea 素材）
- [x] LocalSend v2 发送链路：UDP 组播发现 / register / prepare-upload / upload
- [x] 文件发送（PSV → 手机 / PC，HTTP 明文与 HTTPS 加密均支持）
- [x] TLS 加密传输（mbedTLS 3.6.5，含 mTLS 客户端身份）
- [x] 文件接收（手机 / PC → PSV）：prepare-upload 弹确认 → upload 流式写盘，兼容无 Content-Length 的 chunked 上传
- [x] 主动扫描：Vita 收不了 UDP 组播，改向局域网 /24 各 IP 主动 HTTP 探测补全设备表

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

### 发送

1. 打开 PSVSend，等待 Wi-Fi 联网（状态指示变绿）。对方设备会通过 UDP 组播 announce 自动出现在设备列表；若对方没出现，按 **△** 手动扫描整个网段补全
2. 选中目标设备 → 进入文件浏览（ux0 目录树，可多选）→ 确认发送
3. 对方接受后，传输页显示逐文件进度，可随时取消

### 接收

1. 对方（LocalSend App / 桌面客户端）向我们发送文件时，PSV 自动弹出接收确认页，显示来者、平台、文件清单与总大小
2. 焦点默认在 **Accept**：确认后对方即可开传；**Setup** 进入接收设置，可逐文件取消勾选（未勾选的跳过，不出现在接受回执里）；**Reject** 拒绝（对方收到 403）
3. 确认后进入接收进度页，显示逐文件进度与总进度，可随时取消
4. 收到的文件存入 `ux0:data/psvsend/downloads/`，边收边写（`.part` 临时文件，收完改正式名）；与已有文件同名时自动在扩展名前加 `(k)` 序号，不覆盖。中途断流/校验失败会删除残 `.part`，下次启动也会清扫上次异常遗留的临时文件

### HTTPS / mTLS 说明

发送端按对方 announce 的 `protocol` 自动选择明文 HTTP 或 HTTPS。2026 版官方 LocalSend（Rust 内核）对无浏览器会话的发送方强制要求**客户端证书**（mTLS），不出示证书直接握手失败（`certificate_required`）。为此 PSVSend 随固件内置一张设备身份证书（自签 RSA-2048，见 `src/id_cert.inc` / `src/id_key.inc`），HTTPS 连接自动出示；同时按对方 announce 的指纹（证书 SHA-256）锁定服务端证书，不依赖 CA 链。接收端只校验设备证书本身有效，无需信任本客户端。

## 边界与已知限制

以下为当前实现的实际边界，均来自真机验证或代码常量。设计取舍与路线见 [docs/design.md](docs/design.md)。

### 网络 / 发现

- **只走 IPv4、协议 v2**，不实现 v3（签名 / WebRTC / PIN）与 IPv6
- **Vita 收不了 UDP 组播**：系统保留 53317（bind 报 EACCES），而 LocalSend 组播固定发往 `224.0.0.167:53317`，无法 bind 即无法收包。设备列表靠两条替代路径填充：对方主动 HTTP register（每 ~5s）+ 手动主动扫描（设备列表按 **△**）
- **主动扫描只覆盖 /24 子网**（假设掩码 255.255.255.0）：非常规子网（/16、/23 等）扫不全；每 IP 预算 connect 180ms + 响应 500ms，整轮最坏约 1~2 分钟，界面有进度
- **设备表 90s 无动静移除**：对方停止 announce/register 90s 后从列表消失
- **HTTP 服务器单线程顺序处理**：一次只服务一个 TCP 连接，大文件 upload 期间其它请求（对方的周期 register、下一文件 upload）在 backlog 排队。多文件传输不受影响（对方串行等待应答），但传 >90s 的大文件期间对方 register 应答被延后，其设备条目可能因 90s 过期短暂消失、处理完后又回来——传输本身不受影响

### 接收

- 多文件接收中途取消的竞态已修复（收满优先于取消判断）：正在传的文件若已收满则保留完整、会话以「用户取消」收尾，未收满才清理其残 `.part`；已收完的文件始终保留
- **接收端只监听明文 HTTP**（候选端口 4567/53318/…，announce 声明 `protocol:http`）；HTTPS 仅用于 PSV 作为发送方时。对方会按 announce 自动走明文
- **同一时刻只处理一个接收会话**：进行中或结束未清场时，新的 prepare-upload 返回 409
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

- 暂无已知问题

## 目录结构

**随仓库分发：**

| 路径 | 说明 |
|------|------|
| `src/` | 源码（含发送客户端与内嵌设备身份证书） |
| `sce_sys/` | LiveArea 素材（图标 / 背景 / 启动图） |
| `fonts/` | 界面内嵌字体：Droid Sans（拉丁）+ Droid Sans Fallback Full（CJK），均来自 AOSP（Apache-2.0），编译时打进 VPK `app0:/fonts/` |
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
│   ├── http.c                  # HTTP 服务器：register / info 入表 + prepare-upload / upload / cancel 接收路由
│   ├── transfer.c              # 发送客户端：prepare-upload / upload（HTTP + HTTPS）
│   ├── receive.c               # 接收会话：确认/拒绝、流式写盘 downloads/、断流清理
│   ├── scan.c                  # 主动扫描：向 /24 网段逐 IP HTTP 探测补全设备表
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
