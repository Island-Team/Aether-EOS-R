# 检查 EOS 证据连线

[English](../../../docs/eos/usage/routed-edges.md) · [首次使用](README_cn.md) · [所有者契约](../interfaces/hardware_cn.md#r01-eos-本地证据连线的实时路由目标契约)

**R01 已在本主题分支实现。** IsOS 负责图准入和 Apply。EOS 通过每个 gate 的路线传递已准入的本地相机证据。仅限所有者的套接字提供当前运行时代的 `route_status`。

先安装并验证所需软件包，在 IsOS 图中选定相机和 gate，然后按[首次使用](README_cn.md)校验并应用。[G06 证据夹具](../../../tests/fixtures/edge_contract/valid_evidence.json)展示精确的 `@v0.1` 相机 frame 到 gate frame，设置 `priority:-20` 和 `frequency_hz:15.0`。IsOS 在 Apply 前拒绝固定执行器、安全、结果、健康及停止路径上的 QoS。Apply 成功只表示声明已准入，不能证明相机健康或帧已传递。

启动部署并先查看 IsOS 权威状态及当前运行，再读取 EOS 路线。仅限本地所有者的运行时 Unix 套接字将接受 `{\"op\":\"route_status\"}`。从当前部署找到套接字，勿使用旧图修订的路径。响应的 `projection_sha256` 表示 EOS 投影字节。夹具路线应返回 `delivery_state:\"active\"`、`routed:true`、`priority:-20`、`frequency_hz:15.0`、`weight:40` 和 `queue_class:\"latest\"`。已配置但未实现的 EOS 本地连线返回 `not_routed` 且调度字段为 null；固定治理路径返回 `fixed_not_scheduled`。旧版 EGIA guard → EOS Dispatcher 跨 core 设计声明不在 EOS 状态中，由 IsOS 图检查器显示。无论是否出现状态行，都不表示设备健康。路线计数显示传递和间断；空队列或传递数为零自身不代表就绪失败。

检查过载时，在同一个运行时代比较 `delivered_count`、`cap_wait_count`、`replaced_count`、`overflow_count`、`consumer_error_count`、`cancelled_count` 和 `gap_count`。限速可能增加等待但不造成丢失。新图像替换保留图像会增加替换及间断计数。停止时关闭证据准入，在等待证据线程前调用 Dispatcher 紧急停止，并取消排队证据、记录间断。停机期间及结束后所有者套接字不再提供 `route_status`；停止前检查计数，之后使用 IsOS 状态和运行事件。命令结果、健康和停止由原有所有者负责，不进入这些队列。用 IsOS 的当前运行日志找到首个就绪或故障事件，再与所有者状态关联；不能从路线行推断健康。运行时重启后计数重置；即使投影摘要不变，也须用 IsOS 运行及进程身份区分不同代。

若路线为 `not_routed`，按[契约](../interfaces/hardware_cn.md#r01-eos-本地证据连线的实时路由目标契约)检查精确的源和目标类型 ID、端口、数据类型及传输。相似或贡献类型不会自动获得传递。缺少已编译端点或实时路线过多导致启动被拒绝时，查看 IsOS 的所有者故障报告，改图期间保持已保存字节不变。[调度设计](../design/runtime/scheduler_cn.md)解释有界队列和公平性规则。
