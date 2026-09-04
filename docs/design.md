# PSVSend 设计文档

> 面向 PS Vita 的 LocalSend 协议（v2）兼容客户端。本文件记录前后端架构、关键决策与开发路线。

## 1. 项目目标

- 在 PSV 上实现 LocalSend v2 兼容客户端，与局域网内其他 LocalSend 设备（手机 / PC）互相收发文件
- 协议目标 **v2**（理由见 §3）：v1 已废弃；v3 的签名 / WebRTC / PIN 体系复杂，PSV 资源有限，且官方客户端同时兼容 v2/v3，v2 生态覆盖最广

### 实现现状（2026-09）

- **已落地（发送方向）**：UDP 组播发现 / register 入表 → UI 选设备、浏览 ux0 选文件 → `transfer.c` 按对方 announce 的 protocol 走 HTTP 明文或 HTTPS（mbedTLS 3.6.5、锁定 TLS1.2）执行 `prepare-upload` / `upload`，带进度回写与本地取消。HTTPS 连接自动出示内嵌设备身份证书（mTLS，应对 2026 官方 Rust 内核接收端强制客户端证书），并按对方指纹锁定其证书（详见 §3 决策表）。
- **已落地（接收方向）**：对方 prepare-upload → 自动弹接收确认页（Accept / Setup 逐文件勾选 / Reject）→ upload 由 http.c 提供流读回调、receive.c 边收边写 `ux0:data/psvsend/downloads/`（先 `.part` 收完改名，sha256 可选校验，断流/校验失败删残留，开机清扫遗留）。兼容对方无 Content-Length 的 **chunked 流式上传**（http.c 内嵌解码状态机）；多文件同会话逐 POST upload。取消/放弃/空闲超时等状态经快照接口给 UI 展示结束原因。announce 已声明 `download:true`。
- **已落地（发现补充）**：Vita 收不了 UDP 组播、也绑不了 53317，设备表主要靠对方主动 register 与 `scan.c` 主动 HTTP 扫描（向 /24 各 IP 的 53317 POST register 拿 member info），UI 三角键手动触发。
- 实现边界的完整清单（单会话 409、清单 32 文件/8KB、超时 60s/120s/30s 三档、接收仅明文 HTTP、单线程顺序处理连接、/24 扫描范围等）见 README「边界与已知限制」。
- **未落地**：与 §4/§5 目标架构的规划差项（session.c 拆分、multipart 收件、接收设置页改名/选目录等）仍在路线中；中文字体已落地（见 §5.5），不再在列。
- 本文按"目标架构"描述，部分命名与实际源码不同（如目标 `http_server.c` / `http_client.c` 实际为 `http.c` / `transfer.c`）；现状与源码布局以 README「目录结构」为准。

## 2. 总体架构（前后端分层）

```
┌─────────────────────────────────────┐
│  前端层（UI）：vita2d 界面             │ 只负责展示与交互
│  设备列表 / 文件浏览 / 弹窗 / 进度     │
└───────────────┬─────────────────────┘
                │ 调用 api.h 接口 + 后端回调事件
┌───────────────┴─────────────────────┐
│  后端层（协议核心）：                 │ 纯逻辑，不碰 UI
│  UDP 发现 / HTTP 服务器 / HTTP 客户端 │
│  会话管理                            │
└───────────────┬─────────────────────┘
                │ 系统 API 调用
┌───────────────┴─────────────────────┐
│  底层：SceNet / SceIo / SceLibJson /  │
│        SceKernelThreadMgr（系统固件） │
│  + vita2d / freetype / mbedtls（第三方）│
└─────────────────────────────────────┘
```

- 底层能力（socket、文件 IO、JSON、线程、GPU）由 **PSV 固件系统库**提供真实实现；**vitaSDK** 提供头文件、stub 库（链接通道）与 newlib（标准 C 库）
- 前后端通过 **api.h 契约**解耦：后端不关心按钮怎么画，前端不关心协议细节，可独立开发与验证

## 3. 关键决策

| 决策 | 结论 | 理由 |
|------|------|------|
| 协议版本 | **v2** | v1 废弃；v3 复杂（nonce 签名 / WebRTC），官方保持 v2 向后兼容，生态主流是 v2 |
| 传输 | HTTP 明文与 HTTPS 双支持，按对端 announce 的 protocol 自动选择 | 明文互通无碍；对端为 https 时必须加密 |
| TLS 方案 | **mbedtls**（工具链自带 3.6.5），锁定 TLS1.2 | PSV 系统 SceSsl 对 homebrew 不可用；TLS1.3 的证书校验不走 per-cert 回调，指纹 pin 依赖 1.2 |
| HTTPS 信任模型 | 对端证书按 announce 指纹 SHA-256 **pin**（VERIFY_OPTIONAL + 自管校验回调），不依赖 CA 链 | LAN 内无公开 PKI；协议本身即以 fingerprint 标识设备 |
| mTLS 客户端身份 | 内置自签 RSA-2048 设备证书（`identity.c` + `id_cert.inc` / `id_key.inc`），对端强制客户端证书时自动出示 | 2026 官方 Rust 内核接收端对非浏览器发送方强制客户端证书，无证书即 `certificate_required` 握手失败 |
| HTTP/1.1 范围 | **子集**：POST、Content-Length、**chunked 解码**、multipart boundary、query string；响应后关闭连接 | 目标是"兼容 LocalSend"，非"完备 HTTP"。chunked 必须支持（官方 Dart 客户端可能使用）；解析器独立模块、防御式实现（全部读取设上限、不支持格式优雅拒绝 400） |
| 内存约束 | 按 **256MB** 设计基准（实际预算 365MB 封顶） | 512MB 为**统一内存**（CPU/GPU 共享），系统保留约 147MB，应用可拿最大 365MB（工具箱可调 256/285/333/365） |
| 文件处理 | **大文件永不整读入内存**，一律流式（收：边收边写盘；发：边读边发） | 内存红线 |
| 传输分块 | 16~64KB 缓冲 | PSV socket / sceIoRead 的大块缓冲限制 |
| 应用数据目录 | `ux0:data/psvsend/`（含 `downloads/` 接收目录），首次启动 `sceIoMkdir` 幂等创建 | 符合 PSV 惯例，homebrew 有写权限 |

## 4. 后端设计

### 4.1 模块划分（单向依赖，无环）

```
src/
├── api.h          # 前后端契约：接口 + 回调
├── net.c          # 网络初始化（加载 SCE_SYSMODULE_NET、sceNetInit、资源池）
├── discovery.c    # UDP 发现：announce 发送 + 组播监听 + 设备表
├── http_server.c  # HTTP 服务器：路由 + multipart 解析 + 流式写盘
├── http_client.c  # HTTP 客户端：register / prepare / upload / cancel
├── session.c      # 会话管理：状态机 + 文件清单 + 进度
└── json_util.c    # JSON 生成/解析（封装 SceLibJson 或手写简单版）
```

### 4.2 核心数据结构

```c
// 设备表（发现线程维护，mutex 保护）
typedef struct {
    char ip[16]; int port;
    char alias[64], fingerprint[64], protocol[8]; // http/https
    uint32_t last_seen;      // 超时未更新移除（如 60s）
} Device;

// 会话（收发共用一套结构）
typedef struct {
    int  id;                 // sessionId
    int  direction;          // INCOMING(收) / OUTGOING(发)
    char peer_ip[16];
    FileEntry files[];       // 每文件: fileId、token、name、size、已收字节、状态
    int  state;              // AWAIT_CONFIRM → TRANSFERRING → DONE / CANCELLED
} Session;
```

### 4.3 线程模型

| 线程 | 职责 |
|------|------|
| UI 主线程 | 渲染 + 消费事件队列刷新界面 |
| 发现线程 | UDP recvfrom 循环 + 定时发 announce |
| HTTP 服务器线程 | accept 循环，**每连接一个工作线程**（LocalSend 并行传多文件） |
| 定时器（可并入发现线程） | 设备超时清理、announce 周期触发 |

共享数据（设备表、会话表）统一 mutex 保护。

### 4.4 前后端契约（api.h）

```c
// 前端 → 后端
void api_start(void);                        // 初始化网络 + 启动各线程
const Device *api_device_list(int *count);   // 在线设备
int  api_send_files(const char **paths, int n, const Device *target);
int  api_accept_session(int session_id);
int  api_reject_session(int session_id);
int  api_cancel(int session_id);

// 后端 → 前端（回调，前端注册）
void on_devices_changed(void);               // 刷新设备列表
void on_incoming_request(const Session *s);  // 弹确认框
void on_progress(const Session *s);          // 进度
void on_session_done(const Session *s);      // 完成 / 失败
```

### 4.5 HTTP 服务器要点

- 路由：`/api/localsend/v2/register`、`prepare-upload`、`upload`、`cancel`、`info`
- `prepare-upload` → 生成 sessionId，为每文件分配 fileId + token，返回 `{sessionId, files:[{id, token, fileName, size}]}`；同时向 UI 抛"待确认"事件
- `upload?sessionId&fileId&token` → 校验会话与 token 匹配 → 流式写盘（先临时名，完成后重命名；同名加序号避免覆盖）
- 安全底线：只收"prepare 过"的 fileId+token，否则 400；拒绝时回 401/403
- 接收文件写入 `ux0:data/psvsend/downloads/`

### 4.6 HTTP 客户端要点

- 流程：register（可选）→ `prepare-upload` 拿 session → 逐文件 `POST upload` → cancel 清理
- 按**接收端 announce 的 protocol / port 字段**决定 http/https 与端口——明文即可互通
- 上传：16~64KB 分块读文件写 socket

### 4.7 实现顺序与验证

1. `net.c` 网络初始化（跑通不崩）
2. `discovery.c` 发现 + 设备表 → **PSV 上能看到手机/电脑设备**
3. `http_server.c` prepare-upload + upload 收文件 → **电脑 curl 模拟 LocalSend 对测**
4. `session.c` 状态机 + 确认/取消
5. `http_client.c` 发送 → **官方 LocalSend App 真机对测**

## 5. 前端设计

### 5.1 页面结构（960×544，掌机）

| 页面 | 内容 | 交互 |
|------|------|------|
| 设备列表（主） | 在线设备（别名、类型） | 选中 → 发送流程 |
| 文件浏览 | ux0: 目录树，多选文件 | 进入 / 勾选 |
| 发送确认 | 目标设备 + 文件清单 + 总大小 | 确认 / 返回 |
| 接收请求确认 | 来者名字/平台 + 文件数 + 预览（含取消态） | 接受 / 设置 / 拒绝；发送方取消 → 单个关闭 |
| 接收设置 | 本次保存目录（只读默认）+ 逐文件改名(占位)/勾选跳过 | 方向键选行、确认勾选、返回继续 |
| 传输进度 | 会话列表 + 进度条 | 取消 |
| 设置 | 别名、确认键布局、端口、主题 | — |

### 5.2 架构：事件驱动 + 状态机

```
主循环（每帧）:
  poll 输入（按键/触摸） → 处理当前页交互
  → 消费后端事件队列（设备变更/收件请求/进度）
  → 渲染当前页 → 帧同步
```

- 后端回调**只塞事件队列**（EV_DEVICES_CHANGED / EV_INCOMING_REQUEST / EV_PROGRESS），主循环消费，线程安全不卡渲染
- 页面为状态机：`enum Page` + 每页自己的选择状态

### 5.3 双输入：按键 + 触摸统一模型

- **输入抽象层**：页面只处理抽象动作 `UiAction`（CONFIRM / CANCEL / UP / DOWN / LEFT / RIGHT / BACK / MENU），不判断物理键
- **按键路径**：十字键移动焦点，确认键触发焦点控件
- **触摸路径**：坐标命中检测 → 触发控件动作
- **控件统一**：`{ Rect bounds; UiAction action; void *target; }`，两种输入源最终走同一 action；可混用
- **确认键布局可配置**：美式（X=确认，默认，港版/国行习惯）与日式（O=确认），存 config；触摸不受影响

### 5.4 主题系统（theme.c）

- 主题 = 一组颜色常量（bg / card / card_hl / text / text_dim / accent / accent_text / success / danger / warn / border），UI 绘制只引用主题变量，**运行时可切换，下帧生效**
- 内置主题（先统一**深色基调**，不做深浅色切换）：
  - **OLED**：纯黑 `#000000` 底 + 卡片 `#111111` + 白字高对比（PSV1000 OLED 最佳）
  - **Yaru**：深灰底 + Ubuntu 橙 `#E95420` 主色 + 圆角扁平卡片（vita2d 矩形拼接画圆角，不依赖图片）
  - **自定义**（后续实现）：色盘选主色 → **RGB↔HSV 推导整套**（背景=主色暗化、卡片=背景+10% 亮度、高亮=主色、文字按对比度取白/深），保证配色协调
- 默认主题：Yaru 深色

### 5.5 字体

**决策（2026-09）：freetype + 内嵌开源字体，替代 PVF 作主渲染路径。** 依据：

- libvita2d 的 `vita2d_font` 即 freetype 封装（内部 `FT_Init_FreeType` / `FT_New_Memory_Face`），支持从文件/内存加载任意 TTF/OTF
- PVF 是固件内置字体（实为 otf/ttf 改名，位于 sa0 系统分区），覆盖范围固定且不可扩：即便用 `vita2d_load_system_pvf` 按语言组合注册（拉丁 + 简体中文），生僻字/非简体字符仍会缺字，只能算零体积过渡方案
- 自带字库才能保证"对方发来任意中文名都能显示"

**实现（2026-09-04 落地）：** 双字体逐段路由，均为 AOSP 字库（Apache-2.0，与项目同许可）：

- `fonts/DroidSans.ttf`（拉丁）+ `fonts/DroidSansFallbackFull.ttf`（CJK/全角，28629 字形），CMakeLists 打进 VPK `app0:/fonts/`
- 单字码点 ≤ `0xFF`（ASCII / Latin-1）走拉丁字体，其余走 CJK 字体；`widgets.c` 内按字符分成连续同字体段、整段一次绘制（保留 kerning、控制调用次数）
- freetype 的 y 是**基线**、size 是像素字号；`w_text` 的 y 保持"行首升部线"语义，内部基线放在 `y + 0.81*size`（CJK 满格字形顶比例，实测 glyf yMax/upem；盒 ascender 1.043 含 em 上方行距空白，基线过高会整体偏下）。字号换算 `scale=1.0 → 20px`（`FONT_PX`/`FONT_ASC` 常量集中在 `widgets.c`，整体缩放只改一处）
- **性能事实 + 按字号分槽（2026-09-04 晚落地）**：libvita2d freetype 后端自带**字形级 atlas 缓存**（FTC_ImageCache + 共享 texture atlas），同一字体对象内字形只栅格化一次、后续直接 blit；但 atlas 缓存**不分字号**——同一字形先以小字号缓存、再以更大字号绘制时会把小位图整体放大（draw_scale≠1），槽位间渗色一并放大，表现为大字笔画不均、横向条纹。这是真机"大字横线/参差"的根因，故改为**按像素字号分槽**：`ui.h` 暴露 `font_get(size, cjk)`，`ui_main.c` 按字号懒加载独立字体对象（各自 atlas、draw_scale 恒为 1），对 `widgets.c` 绘制 API 透明。早期"需字符串级缓存"的顾虑依旧不成立
- 字库用纯 CJK fallback 单字体不够（无 ASCII），故拉丁/CJK 必须成对；字体本身无 GPL/许可冲突
- 缺字行为：字符无字形时不渲染（atlas 添加失败即跳过），不会有方块/乱码占位

**校准状态（2026-09-04 真机确认）：** 升部按字形实测为 `FONT_ASC=0.81`（盒 ascender 1.043 不用，理由见上），字号基准 `FONT_PX=20px@scale1`，配合按字号分槽绘制后大字/小字均显示正常；若个别场景仍需微调，改 `widgets.c` 里这两个常量即可。`w_text_w` 返回行高 h=字号（近似）用于垂直居中，如需精确按字形度量再改。

**后续扩展：** 可加"从 `ux0:` 加载自定义字体替换/追加"（当前固定内嵌两份）。

### 5.6 文件组织

```
src/ui/
├── ui.h          # 页面枚举、AppState、字体句柄 font_get(size,cjk)、Input 抽象
├── ui_main.c     # 主循环：输入 → 事件 → 渲染 + 字体按字号分槽（font_get 实现）
├── theme.c/.h    # 主题颜色表 + 切换（theme.h 定义 theme_t）
├── widgets.c     # 文本/列表/按钮/进度条/弹窗控件（FONT_PX / FONT_ASC 常量）
├── input.c       # 按键 + 触摸 → 抽象动作
└── pages.c       # 各页面渲染与交互（设备列表/文件浏览/收发确认/进度/设置）
```

### 5.7 前端开发顺序（独立可测，不依赖后端）

1. UI 框架：主循环 + 页面切换 + 控件基础 + **theme.c（OLED/Yaru）**
2. 设备列表页（假数据）
3. 文件浏览页（读 ux0 真目录）
4. 确认弹窗 + 进度条
5. 接后端事件队列，换真数据
6. 设置页（含主题切换；自定义色盘后续）

## 6. 配置与存储

- `ux0:data/psvsend/config`（JSON）：alias、confirm_layout（0=美式 / 1=日式）、port、theme
- `ux0:data/psvsend/downloads/`：接收文件
- 首次启动 `sceIoMkdir` 幂等创建（已存在返回 0x80410011，忽略）

## 7. 开发路线

- **阶段 A（后端）**：net.c → discovery → http_server → session → http_client
- **阶段 B（前端框架）**：主题 + 控件 + 页面切换 → 设备列表 → 文件浏览 → 弹窗/进度
- **阶段 C（对接）**：前端接后端事件队列，真机联调
- 每步有可验证里程碑（见 §4.7 / §5.7）

## 8. 风险与注意

- multipart 边界解析（上传 body 格式以协议文档为准）
- 大文件流式写盘（内存红线）
- 并发多文件上传（一个 session 多连接）
- 网络异常处理：断连、超时、设备休眠、飞行模式恢复
- 内存监控调试：`mallinfo()`（newlib 堆）+ `sceKernelGetFreeMemorySize()`（系统余量）——注意统一内存下显存块（sceKernelAllocMemBlock）不计入 mallinfo，需结合查看
- 传输速度预期：PSV 802.11n 单天线，实测约 10~20MB/s

## 9. 待定问题

- [x] 中文字体：已落地（freetype 双字体 Droid Sans + Fallback，按字号分槽绘制，见 §5.5）；升部实测校准 0.81、字号基准 20px@scale1，2026-09-04 真机确认显示正常
- [ ] 深浅色切换是否做（当前统一深色）
- [ ] 自定义主题色盘的实现时机（先 OLED/Yaru，色盘后置）
- [ ] HTTP 解析兼容清单最终确认（chunked 已定必做）
- [ ] 接收设置页：文件重命名输入（接 PSV 系统键盘 SceIme，或自绘内置键盘；当前仅占位弹窗）
- [ ] 接收设置页：本次保存目录选择（目录浏览/预设；需确认 ux0 目录权限；当前固定 `ux0:data/psvsend/`）
- [ ] 接收页"验证"功能（LocalSend 的验证码/校验交互）当前不做，等真实协议接入后再定
- [x] 多文件接收中途取消的竞态：已修复（收满优先于取消判断）——正在传的文件若已收满则以「用户取消」收尾并保留完整，未收满才清理其残 `.part`；前 N-1 个完整文件始终保留（细节见 README「边界 / 接收」）
