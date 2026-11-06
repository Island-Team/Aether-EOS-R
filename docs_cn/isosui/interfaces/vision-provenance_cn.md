# Vision 来源显示——Q01 实现

[English](../../../docs/isosui/interfaces/vision-provenance.md) · [工作区面板](workspace-panels_cn.md)

IsosUI 拥有 EOS Vision 帧的浏览器显示。在打开 EOS Vision WebSocket 前，它读取 IsOS 当前 `process-status` 的运行 ID、已保存和活动图版本，以及非秘密 `vision_evidence` 投影。EOS 拥有帧元数据与 JPEG 字节；IsOS 拥有所选 gate/来源/渲染绑定；EGIA 拥有独立的类型化检查。UI 不将显示的图像当作执行器结果。

Q01 中，`process-status.vision_evidence` 须在当前已知运行中包含选定 `gate_path`、`source_path`、`projection_revision` 与 `{max_width_px,jpeg_quality}`，且已保存版本等于活动版本。UI 将该 `projection_revision` 作为现有订阅 `revision`，并发送这些精确渲染参数。IsOS 活动版本仍是已保存整图摘要，区别于 EOS 投影摘要。EOS 增量 `subscribed` 响应须证明 `graph_revision`、`projection_revision`、`runtime_stream_id`、`gate_path`、`source_path` 与当前 `gate_generation`。只有来自所选 EOS 端点、gate/来源/版本与 IsOS 当前选择一致时 UI 才接受。浏览器不接收 EGIA 的私有授权或端点。旧的 `{type:"subscribed"}` 响应不能建立 Q01 证据；面板保持“尚无新画面”，并显示明确的拒绝原因，直到收到可验证的订阅证明。

每个 Q01 二进制帧使用 EOS 小端 `u32` 元数据长度、最多 4096 UTF-8 JSON 元数据字节和最多 8 MiB JPEG 字节。UI 拒绝零长度/超限/截断元数据、畸形 JSON、非法尺寸、JPEG SOI/EOI 无效、JPEG 超限以及对实际收到 JPEG 字节计算所得 `jpeg_sha256` 不匹配。必填 `graph_revision`（64 位小写已保存整图摘要）、`projection_revision`（64 位小写 EOS 投影摘要）、`runtime_stream_id`（UUID）、`source_path`（规范相机路径）、`source_frame_id`（非空 EOS ID）、`gate_path`（选定 gate）、`gate_generation`（正无符号 64 位）、`render`（选定宽度/品质）与 `jpeg_sha256`（64 位小写十六进制）。`sequence` 和 `timestamp_unix_ms` 可缺省。交付身份为 `(graph_revision, runtime_stream_id, gate_path, gate_generation)`；源序号本身不足以证明同一交付。不同渲染参数可使同一交付的 JPEG 摘要不同，因此 Q01 与 EGIA 比较字节时须使用相同所选参数及校验后的摘要。

UI 只接受在当前已证明 stream 中严格大于上次显示值的 generation。仅凭二进制帧不得重置基线。新 stream 须经所选端点重新订阅握手，新 IsOS 运行须重新读取 process-status 并订阅。Stop 意图、运行/版本/选择变化、socket 关闭、隐藏/卸载或 process-status 丢失时，UI 撤销显示 generation token、关闭 socket、移除当前 JPEG，并分别显示 `stopped`、`stale`、`disconnected` 或 `evidence unavailable`。该 token 改变后，延迟的解码或摘要 Promise 不能重新绘制。畸形、重复、较旧、错误版本/来源/gate/stream 帧被拒绝，不推进显示 generation；须显示带稳定本地原因的明确**帧被拒绝**诊断，不能显示空白成功帧。健康 stream 上的 `no new frame` 与拒绝、断开、缺少绑定证据区分。EOS 停机后不得发送缓存帧。

解码器现将元数据限制为 4096 字节，在显示前校验所选所有者字段与 JPEG 摘要。面板显示所选来源、gate、运行、stream、generation、摘要、订阅上限、实际帧率及本地观察到的跳过代次；不把它们称作 EOS 路由器的全局限频或丢帧总数。聚焦测试覆盖成功和拒绝决策，以及挂载后的 Stop／延迟摘要边界。所选版本的浏览器回放仍属 Q01 验收工作。此显示没有图像上传、提示或执行器路径。

**设计轮验证，2026-11-02：** `npm ci`、`npm test`（179 项通过）及 `npm run build`（本地与托管）通过。变更页面的本地链接只读检查和 `git diff --check` 通过。完整输出在忽略的 `logs/test/20261102-q01-camera-design/`。这些检查验证现有 UI 基线；目标显示围栏和 Q01 回放尚未实现。

## 公共浏览器端点

IsosUI 只连接选定 EOS 浏览器 bridge origin 上现有的**公共 loopback TCP** `WS /api/eos/v2/vision/ws`；其 `GET /api/eos/v2/vision/gates` 提供 EOS 投影 `revision`。它从不打开 `eos-vision-owner.sock` 或 EGIA 检查 UDS，也不接收 EOS 订阅授权或独立的 EGIA 检查授权。IsOS `process-status.vision_evidence` 投影只提供选定 gate、来源、投影版本、固定 `{max_width_px:1280,jpeg_quality:80}` 与 `target_fps:30`，以及当前运行与活动已保存整图版本；没有端点或授权路径。UI 在公共 `subscribe` 中发送这些精确渲染／速率值。公共 `subscribed` 证明中 `gate_generation:0` 表示 stream 已准备好但**尚无帧**，不是图像。错误 gate／来源、已保存／投影版本、stream 或之后的 generation 在替换显示的 JPEG 前拒绝。浏览器路径继续检查 Host 与 Origin；`Authorization` 标头或所有者授权在此路径无效。Stop 与运行变化清空显示帧，即使旧公共 socket 短时仍可读取。

固定的 [IsosUI Q01 用例](../../../ui/tests/fixtures/q01_vision_display/README_cn.md) 覆盖有效显示、首帧前握手、重复／较旧 generation、错误版本／来源／stream、摘要失败、缺少选择和停止后的延迟结果。它们是设计 fixture，不是浏览器回放证据。

**契约修订验证，2026-11-02：** `python3 tests/fixtures/q01_vision_display/check_cases.py` 校验了 12 个固定用例。`npm test`（179 项通过）、`npm run build`（本地与托管）及 `git diff --check` 通过。变更页面的链接通过只读检查。完整命令输出在忽略的 `logs/test/20261102-q01-contract-repair/`；不声称已完成现场浏览器帧回放。

**实现验证：** `npm test`、本地与托管 `npm run build`、fixture 结构检查及 `git diff --check` 的完整输出保存在忽略目录 `logs/test/20261103-q01-vision-ui/`。挂载测试使用 EOS socket stub；跨所有者 Q01 回放仍待完成。

**摘要乱序修复，2026-11-03：** 面板在 SHA-256 完成后、替换图像前再次比较当前代次。挂载测试先完成代次 2、后完成代次 1，确认显示保持为代次 2。`npm test` 的 41 个文件共 186 项通过；`npm run build` 的 TypeScript、本地与托管包及 manifest 校验通过；12 个设计 fixture 的结构检查和 `git diff --check` 通过。完整输出位于忽略目录 `logs/test/20261103-q01-vision-ui-race-repair/`。
