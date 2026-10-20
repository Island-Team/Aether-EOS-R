# 独立模拟器接口

[English](../../../../../docs/eos/develop/interfaces/external/simulator.md)

`eos-simulator` 是显式启动的独立进程，拥有 Demo world、单调时钟、地图、射线检测、媒体 解码、持久化和浏览器 API。`eosd` 不启动、不监管它，也不持有 simulator world 状态。 停止或重启模拟器不得终止 EOS。

## 进程边界

| 接口 | 默认位置 | 使用方 |
| --- | --- | --- |
| Unix socket 上的私有 gRPC | `<run>/simulator.sock` | `eosd` 中的 Demo adapter |
| HTTP 与 WebSocket | `127.0.0.1:16004` | WebUI |

私有 `eos.simulator.v1.SimulatorService` 提供有界的 `Health`、`Execute`、 `ReadTelemetry` 与 `ReadCameraFrame`。Socket 仅 owner 可访问，调用使用有限 deadline。 Socket 缺失或断开时，Demo 设备报告 unavailable、命令被拒绝、相机不返回 frame；后续调用 会重连，EOS 的其他服务继续运行。

浏览器不能提交设备状态、camera pixel、detection、模型响应、持有状态或 primitive 结果； 这些始终是 simulator/EOS 的输出。

HTTP 与 WebSocket 连接共享 64 个 session worker 的硬上限。请求解析在 worker 中执行而不阻塞 accept loop；已完成的 worker 会在 slot 复用前被回收；所有 slot 占用时，新连接留在 kernel backlog 中。关闭服务时会先关闭所有已接纳 transport，再 join worker，从而中断等待中的读写。

## 配置与生命周期

`etc/eos/sim.toml` 使用严格键：

```toml
[simulator]
host = "127.0.0.1"
port = 16004
map_id = ""
camera_source = "scene"
```

`host` 必须是 loopback，配置文件使用保留的默认端口 `16004`。开发工具可以传入 `eos-simulator --port N`；默认端口被其他本地进程占用时，`eos-launch.sh` 会自动这样做。`map_id` 选择 simulator state root 下的稳定地图；空 ID 使用内存白色 fallback。`camera_source` 只能是 `scene` 或 `system`。提交的 `scene` 配置无需主机相机，使用下文基于射线选择 source library 的行为。选择 `system` 时需要 GStreamer 的 `gst-launch-1.0`，以及提供 `autovideosrc`、`videoconvert` 和 `jpegenc` 的插件；它打开操作系统默认相机，并将其作为 simulator 唯一的 camera source，此时不会记录 source-line 命中或 selected source ID。相机缺失或中断时会持续重试，在新 JPEG 到达前 Demo camera 不返回 frame。更改 active map 或 camera source 需要重启 `eos-simulator`；提交 active map 的 manifest 或 raster 会原子热加载。

## V2 持久化

Clean-break 布局位于 `<state>/simulator/`：

```text
simulator/
├── maps/<map-id>/manifest.json
├── maps/<map-id>/raster.png
├── library/index.json
└── library/files/<managed-file>
```

旧 `<state>/demo/maps/*/assets` bundle 不会被读取、迁移或删除。路径由服务生成并规范化， 客户端不能保存任意 host path。

地图 manifest 包含 `version = 2`、稳定 `id`、无符号 `revision`、正的 raster 尺寸和分辨率、 origin 以及 `lines`。每条 line 包含稳定 `id`、全局 `source_id`、平面 `center`、有限 `yaw_rad` 和正 `length_m`。`source_id` 必须引用 `library/index.json`；地图 manifest 不嵌入 媒体 metadata 或路径。ID 使用 store 的有界语法，绝不解释为路径。

全局 source index 有自己的 revision，记录稳定 source ID、准入 media type、managed filename 和 byte size。任意地图仍引用 source 时拒绝删除。Manifest、raster、source 与 index 更新都先完整校验，再通过 fsync-and-rename 原子提交；失败时旧 revision 仍可读取。

## 浏览器 API

所有 route 位于 `/v1/eos/simulator`；已删除的 `/v1/eos/demo/*` 没有 alias。请求和响应均有界。 修改已有资源必须在 `If-Match` 携带当前 revision；过期 revision 返回 `409`，缺少前置条件 返回 `428`，非法输入返回 `400`，资源不存在返回 `404`。适用的 revision response 返回 `ETag`。

| 方法 | Route | 用途 |
| --- | --- | --- |
| `GET` | `/` | API version 与 active map ID |
| `GET` | `/snapshot` | 原子 live simulator snapshot |
| `GET` | `/raster` | active map 的规范 PNG |
| `GET`, `POST` | `/maps` | 列出或创建地图 |
| `GET`, `PUT`, `DELETE` | `/maps/{id}` | 读取、替换或删除 manifest |
| `GET`, `PUT` | `/maps/{id}/raster` | 读取或替换 PNG |
| `GET` | `/sources` | 全局 source-library index |
| `GET`, `PUT`, `DELETE` | `/sources/{id}` | 下载、上传/替换或删除媒体 |

服务只接纳支持的 image/video media type，并执行 byte、image 与 decoded-frame 上限。提交前 校验 map/source 引用、尺寸、PNG encoding、规范路径与 revision。CORS 仅允许文档规定的生产 origin 和本地 WebUI 开发 origin。

### WebSocket

`GET /v1/eos/simulator/ws` upgrade 为权威 live channel。每个 JSON envelope 都包含 `version: 2` 与 `type`；请求可带 `request_id`，响应会原样返回。新连接收到 `snapshot`； 重连或发现 revision gap 后发送 `resync`。不支持的版本/type、非法或超限消息只返回有界的 结构化 `error`，绝不修改状态。

Snapshot 携带 active map/revision、设备状态，以及视觉射线命中时选择的 line/source。 Map/source commit 发布带 revision 的 event；active-map commit 热加载后发布新 snapshot。 发现 gap 时，客户端用 snapshot 替换本地状态，不跨 gap 重放变更。

## 视觉来源解析

当 `camera_source = "scene"` 时，每个 camera state 都从 base 位置沿 `base.yaw + gimbal.pan` 发出水平射线，与有限来源线求交 并选择最近的正向命中。未命中时 selected ID 为空，Demo camera 不返回 frame。

命中后，模拟器通过全局 library 解析 `line.source_id`，且只打开该规范 managed file。 Image/video 按需解码；video 只在被选择时生成 frame。已解码 source 保存在有界 LRU cache。 来源线是 camera trigger，不是碰撞几何或 semantic detection。EOS 仍把 camera frame 送入 已配置的模型与校验流水线。

当 `camera_source = "system"` 时，可重启的 capture worker 会持续将系统默认相机转换为有界 JPEG frame，并且只保留最新一帧。此模式下 map source line 与 library media 不参与 camera 输出。

## 日志与故障

结构化日志记录启动/关闭、IPC 连接/恢复、WebSocket session 数、map/source commit、热加载、 source selection 变化、decode/cache failure、revision conflict 与有界 shutdown counter。 生命周期与 commit 使用 `info`，状态转换使用 `debug`，可恢复故障使用 `warn`，不可恢复的 服务/存储故障使用 `error`，只有 opt-in timing 使用 `trace`。

不得记录 tick 或成功 frame，也不得记录媒体 bytes、raster pixel、完整 manifest、command payload、credential 或 WebSocket body。重复 disconnect/decode warning 必须去重或限速。
