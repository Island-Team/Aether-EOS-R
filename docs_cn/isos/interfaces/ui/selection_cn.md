# 通用输出数据选择

[English](../../../../docs/isos/interfaces/ui/selection.md) · [呈现 HTTP](../../develop/http/presentations_cn.md) · [发布类型](../packages/release_cn.md)

## 概览

发布贡献的 `DataType` 和已注册输出端口是类型权威。发布验证会检查 ID/版本后缀；owner 注册必须先证明端口与类型绑定，才可将描述符交给选择器。IsOS 为已应用的输出选择公开呈现元数据；生产者拥有载荷字节、编码、端点授权和传送。MIME 标签或 UI 方法本身不能授权解码。不增加第二份呈现清单。

## 类型来源

[节点数据类型](../nodes/data-types_cn.md)是 `id`、`version`、`purpose`、`media_type`、`codec`、`schema` 和 `max_payload_bytes` 字段的唯一权威来源。注册输出端口绑定该类型；归属方描述符再绑定节点路径、端口、表示、交付方式、不透明端点 ID、运行 ID 和图修订。字段见[呈现描述符](presentation_cn.md)。IsOS 只选择元数据，不取得负载字节或端点授权。

## 表示族

JSON 记录和文本使用 `json` 与 `text`；标记为 `time_series` 的 JSON 时间序列使用 `chart`；JPEG/PNG 使用 `image`；有界 RGB/RGBA/depth 平面使用 `canvas`；标记为 `detections` 的 JSON 检测使用 `overlay`；Opus 或 PCM 音频使用 `audio`。Depth 需要单位，生产者帧契约还须定义无效值语义。检测需要坐标系和图像关联；音频需要采样率和声道语义。这些载荷字段属于生产者契约，IsOS 不从类型名称猜测。

## 可用性

当前 IsOS 路由执行输入和 运行／修订检查后返回 503，因为选定 owner 尚未发布已验证公开描述符。精确请求和当前错误见[呈现](../../develop/http/presentations_cn.md)。Stop、Restart 与 Apply 会使先前选择失效。
