# EGIA gate Vision 证据

[Q03 所有者资格验证设计](../design/q03-qualification.md)以重复的 stub stream 测量此现有只读入口。该基准不使用也不扩展一次性 Q01 旧代际测试套接字；所选 Q01 字节和身份比较仍是独立前提。

[English](../../../docs/ei/interfaces/vision-evidence.md) · [阶段契约](pipeline.md) · [托管配置](isos-config.md)

EGIA 拥有名为 `egia.vision.ingress@v0.1.frame` 的只读类型化入口及 Q01 的脱敏检查投影。它是进程拥有的输入，**不是第八个阶段或图中可编辑端口**。IsOS 在图外创建一个选定服务绑定，把应用图中 EOS 相机 gate 的 `preview` 接到这个精确入口；图只提供相机 → gate 边，不包含跨 core 图像边。此选择遵守 G06 不准入任意 `service_bridge` 图边的规则。JPEG 不进入 `resource`、A04 文本样本、模型提示、日志、journal、命令或执行器路径。

## 选定绑定与传输

IsOS 在不可变图版本快照中固定无秘密模板，再在私有运行目录生成 `egia/vision-binding.json` 和模式 `0600` 的所有者授权文件。绑定在一次部署运行中不可变，包含 `schema_version: 1`、`deployment_run_id`（选定 IsOS run ID）、`graph_revision`（保存整图字节的 64 位小写 SHA-256）、`projection_revision`（选定 EOS 投影字节的 64 位小写 SHA-256）、`eos_endpoint`（IsOS 选定的本机所有者 Vision WebSocket 端点，EOS 定义的固定所有者 UDS，不使用 TCP 或硬编码端口）、`source_path`、`gate_path`、`ingress_type: "egia.vision.ingress@v0.1.frame"`、`render`（`max_width_px`、`jpeg_quality`）及绝对授权文件路径。端点和授权由 IsOS 选择，图作者和 EOS 帧元数据不能选择。EGIA 校验端点及授权文件属主/权限，将授权绑定到精确的运行、版本、来源及 gate，且只在 EOS readiness 匹配后连接。EOS 须通过所选端点及所有者授权证明这些字段，二进制载荷才能被信任。浏览器 Origin 或 loopback 地址本身不构成身份认证。每次部署运行轮换 run ID 与授权；同一次运行内 EOS 子进程重启只轮换 `runtime_stream_id`。

生成绑定示例：

```json
{"schema_version":1,"deployment_run_id":"r7-cde","graph_revision":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","projection_revision":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","eos_endpoint":{"transport":"uds","path":"/run/user/1000/isos/v/cf92cf47318cd17e5d636bbff38aeaf8/eos-vision-owner.sock"},"source_path":"eos/hardware/camera/front","gate_path":"eos/interface/gate/front","ingress_type":"egia.vision.ingress@v0.1.frame","render":{"max_width_px":1280,"jpeg_quality":80},"target_fps":30,"grant_file":"/run/user/1000/isos/control-grants/main/r7-cde/egia-vision.grant"}
```

所有者套接字与检查套接字位于独立私有目录 `<runtime-root>/v/<key>/`。`<key>` 是 UTF-8 `<deployment>`、一个 NUL 字节及 UTF-8 `<deployment_run_id>` 的 SHA-256 前 16 字节，以 32 位小写十六进制表示。端点目录及其 `v` 父目录必须属同一 UID 且模式为 `0700`，两个套接字模式为 `0600`。授权及 `egia/vision-binding.json` 仍位于 `<runtime-root>/control-grants/<deployment>/<run-id>/`。EGIA 根据授权路径及所选 run ID 推导端点目录，并拒绝错误路径、属主、权限、符号链接祖先，或相邻 `vision-verifier.json` 中运行、图版本、投影版本、来源、gate、授权摘要不一致的情况。旧的运行根目录套接字布局仍可安全读取。IsOS 在终止子进程前撤销两个相邻校验文件，并在回收子进程后删除套接字和端点目录。

`schema_version` 是必填整数且必须为 1。`deployment_run_id` 是必填的 1–64 字节 ASCII IsOS run ID，须等于受管进程运行绑定。`graph_revision` 是必填的 64 字符小写十六进制摘要，须与所选 `stages.json` 版本和 EOS 活动版本一致。`projection_revision` 必填，须等于所选 EOS Vision 目录和订阅的 `revision`，与 `graph_revision` 不同。`eos_endpoint` 是必填类型化端点对象；UDS `path` 必须是所有者控制的绝对路径，Q01 绑定不允许 TCP。`source_path` 和 `gate_path` 是必填的规范应用图路径，须解析为恰好一条相机 → gate 边。`ingress_type` 必填且精确匹配。`render` 必填且固定为 `max_width_px:1280`、`jpeg_quality:80`；`target_fps` 必填且固定为 30。它们由 IsOS 生成，不是 EGIA 或图的默认值。相同渲染参数允许 Q01 比较 JPEG 字节。`grant_file` 是必填的所有者控制绝对路径；授权字节不进入 JSON、日志、提示或检查输出。未知安全关键字段及冲突身份拒绝。IsOS 不为此绑定传递用户 QoS；EOS 的相机 → gate `frequency_hz` 仍具权威性。

## 帧准入与生命周期

EGIA 用绑定的 gate、`projection_revision` 及渲染参数发送现有 Vision `subscribe` 请求。所有者认证后，`subscribed` 响应证明活动图版本、runtime stream ID、gate、来源及当前 generation。只有元数据为 1–4096 字节的单个 UTF-8 JSON 对象、JPEG 存在且不超过 8 MiB、SOI/EOI 与尺寸有效、`jpeg_sha256` 等于**收到的 JPEG 字节**的 SHA-256，且 [EOS Vision 契约](https://github.com/Island-Team/Island-eos/blob/refactor%28v2%29/runtime-main/docs/eos/interfaces/vision.md) 中所有必填来源字段格式正确时才接受。发布前比较已保存整图版本与 EOS 投影版本、来源、gate、渲染、所选端点、运行授权及已证明的 stream。不可变交付身份是 `(graph_revision, runtime_stream_id, gate_path, gate_generation)`。相机没有序号或时间时，EOS 分配的 `source_frame_id` 仍必填。`gate_generation` 必须为正，并严格大于该 gate/stream 上次接受的值。缺少的源序号/时间显示为缺省，不得补造。

EGIA 最多暂存一张已接受 JPEG 供本地类型化处理，并保留一份有界的纯元数据最新观察。Q01 仅需检查；该帧**不是**七阶段 A04 流水线的 `vision_frame` 输入，不能更新感知、上下文、决策、提示历史、任务状态或安全准入。断开、EOS 停止、撤销授权、运行/图/stream 改变时，EGIA 立即清空 JPEG，并将最新观察标记为不可用。可以保留先前身份供诊断，但只能放在显式标为过时的 `last_accepted` 中。新 EOS stream 必须经重新订阅握手及完整运行/版本/来源/gate 校验，才重置 generation 基线；二进制帧中突变的 stream 拒绝。同一 stream 重连使用上次接受的 generation 作为 `after_generation`，并以已证明 stream 作为 `after_runtime_stream_id`；重复或更旧帧不推进观察。EGIA 停止时取消订阅并在报告 stopped 前清空临时字节；延迟回调不能重新发布。

入口维护当前运行的饱和 `accepted_count`、`rejected_count` 和按原因 `rejection_counts`。它们是 **EGIA 入口**计数，不能代替 EOS `cap_wait_count`、`replaced_count`、`gap_count` 或合成 FIFO `overflow_count`。拒绝是原子的：丢弃 JPEG，不推进最新已接受身份、观察版本、阶段状态、提示或模型调用。稳定原因码为 `binding_invalid`、`endpoint_mismatch`、`grant_denied`、`run_mismatch`、`revision_mismatch`、`source_mismatch`、`gate_mismatch`、`stream_mismatch`、`generation_stale`、`metadata_invalid`、`metadata_too_large`、`jpeg_invalid`、`jpeg_too_large`、`digest_mismatch`、`render_mismatch`、`stopped`。传输丢失设置 `disconnected`，不是一帧被拒绝。原因中不含原始字节、授权内容或自由格式异常文本。

## 只读检查

独立所有者 UDS 上的 `WS /api/aether/egia/vision/evidence` 连接后返回一份纯元数据快照，再发送有界状态变化；不接受动作或图像上传消息。未授权者在升级前收到 HTTP 403 `{"ok":false,"error":"inspection_grant_denied"}`；不发送快照。已连接但没有帧或已断开时返回 `state: "missing"` 或 `"disconnected"`，不得伪造帧。有效快照示例：

```json
{"type":"vision_evidence","state":"available","deployment_run_id":"r7-cde","graph_revision":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","projection_revision":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","runtime_stream_id":"8b4e9b70-5481-4d88-9bd6-4f07cb466ae4","source_path":"eos/hardware/camera/front","source_frame_id":"cam-41","gate_path":"eos/interface/gate/front","gate_generation":7,"sequence":null,"timestamp_unix_ms":null,"width_px":1280,"height_px":720,"render":{"max_width_px":1280,"jpeg_quality":80},"jpeg_sha256":"c8f1b57ba3b319e7c5ddce3dac7f09c729d8d4bce2259c4f03d68cdbb7f69f0d","jpeg_length_bytes":12345,"accepted_count":1,"rejected_count":0,"rejection_counts":{}}
```

`jpeg_length_bytes` 是经摘要验证的 JPEG 收到字节数，为 1 至 8 MiB 的正整数；没有当前帧时为 null。摘要只展示格式，并非省略 JPEG 字节的实际摘要。`type` 和 `state` 是必填枚举；已有选定绑定时，`deployment_run_id`、`graph_revision`、`projection_revision`、`source_path`、`gate_path`、`render` 与计数始终存在。`missing` 或 `disconnected` 时，`runtime_stream_id`、`source_frame_id`、`gate_generation`、可选源 `sequence`/`timestamp_unix_ms`、尺寸、摘要及 `jpeg_length_bytes` 为 null；旧身份只能出现于单独标为过时的 `last_accepted` 对象。计数是当前运行的饱和无符号 64 位整数；`rejection_counts` 只包含上述闭集原因。整条 JSON 不超过 4 KiB。检查输出不含 JPEG、授权、端点路径或模型内容，也不代表执行器动作或阶段证据。

实现契约测试须用 stub EOS WebSocket 覆盖有效帧、可选源字段、畸形长度/JSON/JPEG、两个大小边界、摘要错误、错误端点/运行/版本/来源/gate/渲染/stream、重复及更旧 generation、断开、EOS 重启与新运行。每次拒绝都不得改变观察或阶段/提示状态；停止后不得接受延迟帧。选定 Q01 回放比较 EOS/EGIA 交付身份、校验各渲染摘要，归档元数据与长度但不归档 JPEG 字节。

**设计轮验证，2026-11-02：** `uv sync --extra dev --extra vec`、`uv run ruff check .`、`uv run pytest`（907 项通过）、`uv run pyright`（零错误）、`PYTHONPATH=src:libexec uv run lint-imports` 与 `./bin/islandctl validate` 均通过。变更页面的本地链接和 JSON 示例通过只读解析检查。完整输出位于忽略的 `logs/test/20261102-q01-camera-design/`。这些检查验证现有基线；目标入口与 Q01 回放尚未实现。

## 固定的 0.x 客户端与检查授权——目标设计，尚未实现

IsOS 启用此绑定前，EGIA runtime 的 `--capabilities` 须声明 `vision-evidence-ingress-v1` 与 `vision-inspection-uds-v1`。`EGIA_EOS_VISION_GRANT_FILE` 必须等于生成绑定的 `grant_file` 路径，是同 UID、模式 0600 的常规文件，内容恰好为 64 位小写十六进制加换行；EGIA 不接受图、URI、命令行或 WebSocket 消息中的授权。它仅打开生成的 `eos_endpoint` UDS，使用 `ws://localhost/api/eos/v2/vision/owner-ws`，在 HTTP 升级时发送 `Host: localhost` 与 `Authorization: Bearer <64 位小写十六进制>`，不发送 `Origin` 或 `Sec-Fetch-*`。HTTP 403 `owner_grant_denied`、409 `owner_binding_stale`、503 `vision_unavailable`、超时或错误 socket peer 都在订阅前失败且不发布观察。五秒内首个带掩码文本消息为 `{"op":"subscribe","gate_path":"eos/interface/gate/front","revision":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","target_fps":30,"max_width_px":1280,"jpeg_quality":80}`；同一 stream 重连时另外携带 `after_generation` 与 `after_runtime_stream_id` 两个字段。EGIA 在任何二进制帧前要求 `subscribed` 证明，允许 generation `0`、无 JPEG 的空 ready 握手，并按绑定与选定端点校验两个版本、来源/gate 和 stream。文本错误及随后 WebSocket 1008 或 EOS 1012 清空当前证据。不能回退到直接 gRPC `CaptureFrames` 或 A04 `resource`。

检查使用**另一个** listener 和授权。IsOS 为受管 EGIA 子进程设置 `EGIA_VISION_INSPECTION_SOCKET=<endpoint-dir>/egia-vision-inspection.sock` 与 `EGIA_VISION_INSPECTION_VERIFIER_FILE=<endpoint-dir>/egia-vision-inspection-verifier.json`。EGIA 校验同 UID、模式 0700 父目录，将 UDS 绑定为模式 0600，并读取模式 0600 verifier，其精确字段为 `schema_version:1`、`run_id`、`graph_revision` 与独立 32 字节 `vision-inspection` 授权的 `inspection_sha256`。只有 `GET /api/aether/egia/vision/evidence`、`Host: localhost`、无 `Origin`/`Sec-Fetch-*`、同 UID `SO_PEERCRED` 及 `Authorization: Bearer <64 位小写十六进制>` 可升级；bearer 摘要常数时间比较。错误 bearer 返回 HTTP 403 `{"ok":false,"error":"inspection_grant_denied"}`，过时运行返回 409 `inspection_run_stale`，所有者状态不可用返回 503 `inspection_unavailable`。HTTP 101 后只读流先发送一份有界快照，随后发送状态变化；客户端文本或二进制消息无效并以 1008 关闭。IsOS 在私有运行目录保留检查授权，仅向本机 Q01 verifier 提供路径；公共 EGIA TCP 北向 listener、IsosUI 和 process-status 都不接收它。Stop 在报告停止前关闭 listener 并清除最新当前帧。订阅授权不能认证检查，检查授权也不能订阅 EOS。

固定的 [EGIA Q01 用例](../../../ei/tests/fixtures/q01_vision_evidence/README_cn.md) 要求已接受身份及运行／版本／来源／gate／stream、大小、摘要、重复、断开与停止反例，且阶段或提示不推进。它们是实现输入，不是现场接收证据。

**契约修订验证，2026-11-02：** `python3 tests/fixtures/q01_vision_evidence/check_cases.py` 校验了 18 个固定用例。`uv run ruff check .`、`uv run pytest`（907 项通过）、`uv run pyright`（零错误）、`PYTHONPATH=src:libexec uv run lint-imports`、`./bin/islandctl validate` 与 `git diff --check` 均通过。变更页面的链接与 JSON 示例通过只读检查。完整命令输出在忽略的 `logs/test/20261102-q01-contract-repair/`；不声称 EGIA 已在现场接收图像。

## 已实现的 EGIA 读取器（Q01 所有者切片）

受管的 `bin/egia --capabilities` 探测公布 `vision-evidence-ingress-v1` 与 `vision-inspection-uds-v1`。IsOS 提供四个 `EGIA_*VISION*` 文件／套接字环境变量后，启动过程检查私有目录中的 `egia/vision-binding.json`、运行 ID、`stages.json` 图版本及独立 EOS 授权文件。客户端只连接选定的 EOS 所有者 UDS，验证握手所证明的 stream，并在接受帧前检查来源、gate、两个版本、渲染、尺寸、字节摘要、大小和严格递增的 generation。纯元数据检查快照增加 `jpeg_length_bytes`，表示已验证 JPEG 的收到字节数。断开和停止清空临时 JPEG，并把旧身份标为过时；帧不进入阶段、提示、模型输入或执行器路由。

独立检查监听器由 `EGIA_VISION_INSPECTION_SOCKET` 和 `EGIA_VISION_INSPECTION_VERIFIER_FILE` 配置。EGIA 在模式 0600 的所有者 UDS 上检查同 UID 对端和独立检查授权摘要，升级前拒绝无权访问者，收到客户端消息即关闭。公共 TCP northbound 不暴露此路径。干净停止前移除检查套接字和临时字节。

聚焦实现测试执行 `uv run pytest -q tests/egia/test_vision_evidence.py`；stub 覆盖所有者握手、有效渲染、只读检查及两种授权隔离。完整仓库检查记录在中英文开发指南。选定 IsOS/EOS/EGIA 进程回放与跨所有者字节比较仍属于 Q01 资格验证。

冻结的 `tests/fixtures/q01_vision_evidence/cases.json` 保留 `status: "target_not_implemented"`，表示其 2026-11-02 设计快照；检查器只校验用例结构。当前行为由 `tests/egia/test_vision_evidence.py` 验证；两者都不能替代选定 Q01 进程回放。

## Q01 私有重复代际测试入口

只有精确继承管理器环境值 `EGIA_Q01_STALE_TEST=1` 的选定受管 Vision 子进程才创建此测试入口。EGIA 在读取本地 `.env` 前捕获此值，因此 dotenv 不能启用入口。该标记不进入图、生成配置、能力探针、进程状态或检查投影。EGIA 校验所选运行绑定、EOS 所有者授权及检查验证器后，在检查套接字旁派生 `egia-vision-stale-test.sock`。绝对路径在绑定前必须不存在，父目录非符号链接、同 UID、权限 `0700`，套接字同 UID、权限 `0600`。已有文件或符号链接使启动失败，且不会被删除。每个连接通过 `SO_PEERCRED` 检查与 EGIA 有效 UID 相同。不提供 TCP 或公共路由。没有精确标记时不创建测试套接字，也不保留重复测试元数据。

套接字接收一条以换行结束、最长 1024 字节的 UTF-8 JSON 对象，读取超时两秒，键必须恰为：`schema_version`（整数 1）、`op`（`replay_latest`）、`deployment_run_id`（所选 run ID）、`graph_revision`（64 位小写 SHA-256 十六进制）、`runtime_stream_id`（当前已证明 stream ID）、`gate_generation`（正整数）、`source_frame_id`（当前 EOS 帧 ID）和 `jpeg_sha256`（64 位小写 SHA-256 十六进制）。这些值只用于比较当前已接受状态。调用者不能提供 JPEG、替换元数据、端点、授权、图边或命令。额外字段、错误 JSON 和超限请求返回 `invalid_request`；错误的运行或图修订返回 `binding_stale`；当前 stream、代际、帧 ID 或摘要变化返回 `target_changed`。断连或授权撤销返回 `unavailable`；所有者停止返回 `stopped`。只有匹配当前状态的请求消耗一次机会。

EGIA 仅在测试模式中复制完整的已验证元数据对象，并继续只保留现有的一份瞬时 JPEG。在同一准入锁内重新检查所有者授权和精确目标，取得仅含元数据的 `before` 快照，用该元数据和 JPEG 重建原始帧，经通常的 `VisionEvidence.accept` 校验主体处理，再在普通 EOS 帧无法插入的情况下取得 `after` 快照。成功测试响应是一行 JSON，包含 `schema_version:1`、`op:"replay_latest"`、`injected:true`、`reason_code:"generation_stale"` 及脱敏的 `before`／`after` 检查快照。`accepted_count` 和当前帧身份、摘要、字节长度保持不变；`rejected_count` 和 `rejection_counts.generation_stale` 各增加一。其他响应使用 `injected:false` 和封闭原因码，不带快照。实际重复准入后即使响应写入失败也关闭并删除套接字。绑定后启动失败时，EGIA 关闭监听器。记录自身路径 inode 后，清理仅删除该 inode 并保留任何替换项。若首次路径 stat 在记录所有权前失败，EGIA 不改动该目录项：自身套接字不再监听，留在私有端点目录直至端点目录清理；被替换的 Unix 套接字仍由其创建者管理。断连、stream 变化、授权失效和停止会清除回放元数据；授权失效和停止还关闭套接字。生产检查仍独立且只读。
