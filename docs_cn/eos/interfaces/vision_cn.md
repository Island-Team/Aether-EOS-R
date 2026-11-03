# EOS v2 Vision 接口

[English](../../../docs/eos/interfaces/vision.md) · [软件包节点 IDL](node-contributions_cn.md) · [所有者首次使用指南](../develop/vision-provenance_cn.md)

旧版内置 `eos.interface.camera_gate@v1.0` 是位于 `eos/interface/gate` 的软件节点。必填输入 `frame:camera_frame` 须引用已注册相机的 `frame` 输出；输出 `preview:vision_frame` 是只读流。内置 `isos.ui.vision@v1.0` 位于 `isos/ui/panel`；可选输入 `preview:vision_frame` 选择初始 gate。查看者可选任何活动且兼容的 gate。旧 `isosui.system.vision@v1.0` 仅作为导入别名接受，输出时改为 IsOS 所属类型。这两种节点都不准入执行器命令。旧版 `isos.ui.vision@v1.0` 节点不是 `0.1.0` Vision 目标，其 preview 不是 Q01 EGIA 服务绑定。

## 数据与速率契约

`vision_frame` 使用编解码器 `vision_frame_jpeg_v1`、媒体类型 `image/jpeg`，JPEG 载荷上限为 8 MiB。逻辑 `VisionFrame` 字段：`gate_path`（字符串，规范 gate 路径）、`sequence`（可选无符号整数，源帧序号）、`timestamp_unix_ms`（可选有符号整数，Unix 毫秒）、`width_px` 和 `height_px`（正整数，像素）、`source_rate_hz`（正且有限的数字，赫兹）、`jpeg`（JPEG 字节）。WebSocket 二进制载荷将元数据与 JPEG 字节分开。

Gate 以相机声明的观测频率采集，上限 60 Hz。每个相机输出只保存最新帧；较慢的查看者丢弃旧帧。Vision 节点可选设置 `target_fps`（正整数，帧/秒，上限 60）、`max_width_px`（正整数，像素，上限 4096）、`jpeg_quality`（整数 1–100）调整查看效果。WebSocket 订阅默认值分别为源声明频率（最多 60）、1280 像素、质量 85。当前直接 runtime capture 请求未指定质量时默认 80；需要相同 JPEG 输出的客户端必须显式发送质量。JPEG 编码维持宽高比并使用源 RGB 字节。相机提供质量与完整宽度均匹配的 JPEG 预览时，Vision 复用已校验的字节。只有相机处于 Ready 且 Healthy 时才能取帧。

## 只读浏览器 API

`GET /api/eos/v2/vision/gates` 返回 `{"revision":"<graph-hash>","gates":[{"path":"<gate-path>","label":"Front camera","output_port":"preview","source_path":"<camera-path>","source_rate_hz":30.0}]}`。`revision` 是已载入图字节的 SHA256 小写十六进制摘要。列表只包含活动 gate。`path` 和 `source_path` 是规范图路径，`label` 是显示文本，`output_port` 为 `preview`，`source_rate_hz` 单位为赫兹。

连接 `WS /api/eos/v2/vision/ws`，首先发送一帧带掩码的文本：

```json
{"op":"subscribe","gate_path":"eos/interface/gate/front","revision":"<graph-hash>","target_fps":30,"max_width_px":1280,"jpeg_quality":80}
```

`op`、`gate_path` 和 `revision` 是必填字符串。三个数字选项可省略，范围同上。服务器以文本发送 `{"type":"subscribed"}` 或 `{"type":"error","message":"..."}`。每帧二进制数据依次为小端 `u32` 元数据 JSON 字节长度、该长度的 UTF-8 元数据 JSON、JPEG 字节。元数据含 `gate_path`、`width_px`、`height_px`、`source_rate_hz`；有值时还含 `sequence` 和 `timestamp_unix_ms`。图版本改变或 gate 不活动时拒绝订阅。每个 WebSocket 连接只承载一个订阅。帧上限为 8 MiB JPEG 加 4 KiB 元数据。HTTP 桥接器的浏览器来源限制仍适用。

EOS 在运行时采集和编码；HTTP 桥接器只转发已校验的只读帧。浏览器端点不接受原始相机控制或执行器命令。

## Q01 gate 来源扩展——实现候选

此增量契约对应 [Q01 里程碑](../../island/implementation-todo_cn.md)。直接 gRPC `CaptureFrames` 仍是源快照，不能证明 EGIA 收到 gate 交付帧。下述所有者 WebSocket 在 IsOS 选定绑定活动时携带交付来源。扩展只适用于 IsOS 选定的活动图中 `eos.interface.camera_gate@v0.1` 的交付，不将 `vision.snapshot`、资源或旧 `@v1.0` preview 解释为 gate 证据。

每次成功写入 gate 交付槽时，EOS 分配不可变的**交付身份** `(graph_revision, runtime_stream_id, gate_path, gate_generation)`。`graph_revision` 是 IsOS 管理的活动版本，即已保存整图字节的小写 SHA-256；它不同于现有 Vision `revision`（已载入 EOS 投影字节的 SHA-256）。EOS 只能从已验证的托管启动元数据接受该值。独立 EOS 不能声称 Q01 托管来源。`runtime_stream_id` 是每次 EOS 运行时启动时生成的随机 UUID；即使同一 IsOS 部署运行中子进程重启也必须变化，不能由相机提供。`gate_generation` 是每个 stream/gate 严格递增的正无符号 64 位整数，0 保留不用。达到最大值时，EOS 以 `generation_exhausted` 停止该 gate 的发布，须换新 runtime stream；不得回绕或复用。图改变须重启运行时并换 stream。停机、健康丢失或版本变化时，先清空交付槽；旧 JPEG 不得作为当前帧重放。健康失效会递增来源 epoch；失效前已准备的交付即使在下次读取前健康恢复也不得提交，只有新 epoch 中的新捕获才能发布。

EOS 给每个采集源帧分配 `source_frame_id`：在 `(runtime_stream_id, source_path)` 内唯一的非空不透明 ID，即使相机没有序号或采集时间也必须生成。EOS 在 gate 交付前冻结规范 `source_path`、可选源 `sequence` 与 `timestamp_unix_ms`、尺寸、源字节及此 ID。相机给出的序号与时间仅供诊断，不能替代 EOS ID 或交付身份。gate 快照同时持有源身份和交付身份；HTTP 桥接器不得从较晚的状态读取中拼接元数据。

二进制帧仍使用现有小端 `u32` 元数据长度前缀及 8 MiB JPEG 上限。元数据 JSON 在 JPEG 前按 UTF-8 字节计最多 **4096 字节**；零长度、超限、截断、非对象 JSON 或缺少 JPEG 均无效。现有字段语义不变，新增以下必填字段。

| 字段 | 类型／单位 | 用途、缺省及校验 |
|---|---|---|
| `graph_revision` | 字符串／— | 必填，64 位小写十六进制，须等于 IsOS 活动已保存整图摘要；旧帧缺此字段不能作为 Q01 证据。 |
| `projection_revision` | 字符串／— | 必填，已载入 EOS 投影的 64 位小写十六进制摘要；等于现有 Vision 目录／订阅的 `revision`。 |
| `runtime_stream_id` | UUID 字符串／— | 必填，EOS 启动 ID；重启后不能复用。 |
| `source_path` | 规范图路径／— | 必填，须与选定图边的相机来源一致。 |
| `source_frame_id` | 非空不透明字符串／— | 必填，最多 128 UTF-8 字节；在此源与 stream 内唯一。 |
| `gate_generation` | 无符号 64 位整数／交付次数 | 必填，正数，在 gate/stream 内严格递增；不回绕、不复用。 |
| `render` | 对象／像素及 1–100 品质 | 必填，例如 `{ "max_width_px": 1280, "jpeg_quality": 80 }`，为该 JPEG 使用的已校验订阅参数。 |
| `jpeg_sha256` | 字符串／— | 必填，紧随元数据的**实际 JPEG 字节**的小写 SHA-256；EOS 编码后、不可变帧入队前计算。 |

一个帧的元数据部分示例：

```json
{"gate_path":"eos/interface/gate/front","graph_revision":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","projection_revision":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","runtime_stream_id":"8b4e9b70-5481-4d88-9bd6-4f07cb466ae4","source_path":"eos/hardware/camera/front","source_frame_id":"cam-41","gate_generation":7,"width_px":1280,"height_px":720,"source_rate_hz":30.0,"render":{"max_width_px":1280,"jpeg_quality":80},"jpeg_sha256":"c8f1b57ba3b319e7c5ddce3dac7f09c729d8d4bce2259c4f03d68cdbb7f69f0d"}
```

示例摘要只展示格式，并非省略的 JPEG 的摘要。接收者须对收到的字节重新计算 `jpeg_sha256`。不同订阅可以用不同宽度或品质渲染同一次交付，因此交付身份相同而 JPEG 摘要不同。Q01 回放若声称字节相同，须选相同渲染参数并比较各自校验过的摘要及二进制长度；否则只比较交付身份并分别报告渲染摘要。Q01 中，EOS 从已提交的 gate 快照仅编码一次固定 1280 像素／品质 80 的渲染，将不可变字节与最新槽一同保存；浏览器和所有者 listener 请求该渲染时收到同一字节。摘要只针对该 JPEG 计算一次；槽被替换或清空时丢弃字节。其他渲染选择使用独立字节与摘要。渲染不能改变交付身份或 gate 限速计数。

订阅的 `revision` 仍必填，其含义是 `projection_revision`，不能替代选定 IsOS `graph_revision`。错误或已失效的版本/gate 返回文本错误而无二进制帧。重连时先返回 `subscribed`，EOS 在首帧前从已验证静态路由与实时健康原子地生成证明，在交付后从活动 gate 快照生成；该消息包含 `graph_revision`、`projection_revision`、`runtime_stream_id`、`gate_path`、`source_path` 及当前 `gate_generation`。可选的调用方 `after_generation` 是无符号 64 位游标，若存在则必填等于已证明 stream 的 `after_runtime_stream_id`；EOS 将两者绑定到选定 gate。EOS 只发送严格大于游标的交付。较旧游标可收到当前较新的交付；相同游标等待下一次发布，高于当前值则等待而不重放。不同 stream 的游标以 `cursor_stale` 无效，须无游标重新订阅。没有回绕。旧浏览器客户端可省略游标，它也不赋予命令权限。

IsOS 为一个部署运行选定端点并授予 EGIA 本机所有者只读访问。每次部署运行轮换 `deployment_run_id` 与授权；每次 EOS 子进程启动轮换 `runtime_stream_id`。浏览器 Origin 与 loopback 地址并非进程身份认证。EOS 将授权绑定到选定端点、图版本、gate 与来源；仅靠帧元数据不能授权订阅。浏览器路径继续遵守现有 Origin 规则。EOS 在停止已准入执行器及终止 MuJoCo 前停止新发布并清空待交付/最新 gate 帧；`safety.estop` 和 Stop 维持既有优先级，绝不等待 Vision 订阅者。停机后订阅者收到关闭或不可用结果，不得取得缓存成功帧。

目标原因码：`invalid_metadata`、`jpeg_invalid`、`jpeg_too_large`、`revision_mismatch`、`gate_inactive`、`source_mismatch`、`stream_mismatch`、`cursor_stale`、`generation_exhausted`、`owner_grant_denied`、`stopped`。错误不得包含 JPEG 或秘密授权。`route_status` 仍拥有 `cap_wait_count`、`replaced_count`、`gap_count`，这些是 EOS gate 队列计数，不是 EGIA 入口计数。选定 MuJoCo 测试须证明精确 gate 交付及路由计数；除非实际诱发并观测到替换，不声称正替换数。未有现场 FIFO 图路由前，现有 `overflow_count` 证据仍属合成测试。

**设计轮验证，2026-11-02：** `cargo fmt --all` 与 `cargo fmt --all -- --check` 通过。变更页面的本地链接与 JSON 示例通过只读解析检查。此文档分支未实现 Vision 运行时行为，也不声称已完成选定 Q01 回放。格式检查输出位于本地 `logs/test/20261102-q01-camera-design/fmt.log`。

### 固定的 0.x 所有者传输与握手

现有 `aether-http-bridge` 保留 `--listen <loopback-host:port>`，公共浏览器 `GET /api/eos/v2/vision/gates` 和 `WS /api/eos/v2/vision/ws` 位于其 TCP listener。选定 Q01 运行另外为**同一个 bridge 进程**传入 `--owner-vision-socket <绝对路径>`、既有 `--socket <EOS 私有运行时 socket>` 及 `--run-id <IsOS run ID>`。新所有者 listener 是 IsOS 选定的私有 Vision 端点目录中名为 `eos-vision-owner.sock` 的 Unix socket；该目录可短于控制授权运行目录。端点目录模式为 0700 且 UID 相同，socket 模式为 0600；对应的模式 0600 verifier 位于同一目录。IsOS 在启动前检查两个 Vision socket 路径均符合 Unix 路径长度上限。listener 只接受 `GET /api/eos/v2/vision/owner-ws`、`Host: localhost`、无 `Origin` 或 `Sec-Fetch-*`、无查询参数凭据，且须有 `Authorization: Bearer <64 位小写十六进制>`。不路由公共 gates、control、result 或浏览器 Vision 路径。bridge 只从环境变量 `AETHER_BRIDGE_VISION_VERIFIER_FILE` 读取 verifier 路径，不从命令参数读取；缺失或不安全时启动失败。选定 EOS runtime 的 `--capabilities` 须声明 `vision-provenance-v1`，选定 bridge 须声明 `vision-owner-uds-v1`；本 EOS 实现候选声明这些令牌；IsOS 仍须固定并校验选定候选文件的精确字节。

IsOS 每次部署运行生成新的 32 字节随机 `egia-vision` 授权，以 64 位小写十六进制加换行存于模式 0600 常规文件。独立的模式 0600 verifier JSON 包含精确的 `schema_version:1`、`run_id`、`active_revision`（已保存整图）、`projection_revision`（EOS 投影）、`source_path`、`gate_path` 及 `egia_vision_sha256`（解码授权字节的 SHA-256），不含秘密。bridge 在所有者 readiness 前将 `--run-id`、verifier 属主／权限及各字段与 EOS 运行时私有 socket 的 `vision_attest` 响应比较。响应从已验证托管启动元数据及准入 gate 路由原子构造，包含 `run_id`、`active_revision`、`projection_revision`、`runtime_stream_id`、`source_path`、`gate_path`、`gate_generation` 和 `ready`。首帧前 generation 为 `0`，使用已验证的静态 gate/来源及实时健康，无 JPEG；缺失或不健康的 gate 为不可用。bridge 在 WebSocket 升级前检查同 UID `SO_PEERCRED`，对 bearer 字节求摘要并常数时间比较 verifier。缺失／畸形／错误 bearer 返回 HTTP 403 `{"ok":false,"error":"owner_grant_denied"}`；过时运行／版本／来源／gate 返回 HTTP 409 `{"ok":false,"error":"owner_binding_stale"}`；EOS 或健康不可用返回 HTTP 503 `{"ok":false,"error":"vision_unavailable"}`。Stop 撤销运行授权、关闭所有者 listener 与活动 socket、清空队列帧并删除 socket。每次有界所有者轮询都会重新检查私有 verifier 和 `ready:true` 证明；授权撤销或健康未就绪时，活动 socket 在发送下一帧 JPEG 前以 1012 关闭。私有 IPC 的 `changed:true` generation 须严格大于先前游标，否则 bridge 以 1008 关闭且不发送 JPEG。bridge 或 EOS 子进程在同一运行内重启须重新检查 verifier；EOS 轮换 `runtime_stream_id`。新 IsOS 运行取得不同授权与 run ID。公共 TCP listener 不接受所有者授权。

HTTP 101 有效升级后，首个带掩码文本帧须在五秒内到达，最多 4096 字节，含既有 `op:"subscribe"`、绑定 `gate_path`、EOS `revision`（即 `projection_revision`）及固定渲染／速率选项。可选 `after_generation` 与 `after_runtime_stream_id` **均为此 subscribe JSON 的字段**，须同时出现；前者为无符号 64 位整数，后者为上次证明的 UUID。EOS 在任何二进制帧前按新 `vision_attest` 校验订阅。首帧尚未交付时，文本响应为 `{"type":"subscribed","graph_revision":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","projection_revision":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","runtime_stream_id":"8b4e9b70-5481-4d88-9bd6-4f07cb466ae4","source_path":"eos/hardware/camera/front","gate_path":"eos/interface/gate/front","gate_generation":0}`；已有交付时 generation 可为正。静态身份和 stream 在响应前原子冻结。首消息无效、gate／版本／渲染错误、游标字段不成对或 stream 过时，发送有界文本错误；例如旧 stream 使用 `{"type":"error","reason_code":"cursor_stale"}`，其他情况按需使用 `revision_mismatch`、`gate_inactive` 或 `invalid_metadata`，并以 WebSocket 1008 关闭；EOS 停机以 1012 关闭。`subscribed` 前没有二进制载荷。浏览器 TCP 路径可发送增量证明但没有所有者授权，仍遵守既有 Origin/Host 检查，且不接受所有者授权。

固定的 [EOS Q01 用例](../../../tests/fixtures/q01_vision_provenance/README_cn.md) 约束首帧前 generation `0`、精确 4096 字节元数据和 8 MiB JPEG 边界、游标字段对、旧 stream、错误投影、公共路径授权拒绝与停止行为。它们是设计预期，不是已实现测试结果。

**契约修订验证，2026-11-02：** `python3 tests/fixtures/q01_vision_provenance/check_cases.py` 校验了 12 个固定用例；`cargo fmt --all -- --check` 与 `git diff --check` 通过。变更页面的链接与 JSON 示例通过只读检查。输出位于忽略的 `logs/test/20261102-q01-contract-repair/`。这些仍只是设计证据。
