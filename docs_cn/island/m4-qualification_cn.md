# M4 智能体与工作台完成证据

[English](../../docs/island/m4-qualification.md) · [里程碑](implementation-milestones_cn.md) · [归属方 TODO](implementation-todo_cn.md) · [历史 M4 重放](m4-affected-replay_cn.md)

M4 按已评审的归属方实现及固定版本的安装后重放，关闭原定智能体阶段和操作工作台完成条件。EGIA 管理阶段状态与固定安全路由；IsOS 管理已应用图、进程运行及 Stop；EOS 管理已准入命令与日志；IsosUI 显示归属方状态并提交一条选定原语绑定。本记录不验收公开发行构件或实体硬件切换。

| 完成条件 | 已评审证据 |
| --- | --- |
| 类型化阶段和固定安全路由，包括故障与重启 | EGIA A01/A04/A02/A03 归属方 [A01 #93](https://github.com/Island-Team/Island-egia/pull/93)、[A04 #94](https://github.com/Island-Team/Island-egia/pull/94)、[A02 #95](https://github.com/Island-Team/Island-egia/pull/95)、[A03 #96](https://github.com/Island-Team/Island-egia/pull/96) 及[进程重启 #98](https://github.com/Island-Team/Island-egia/pull/98)覆盖七个阶段身份、格式错误／过期／重复输入、路由证据过载、固定安全与紧急路由、完整 EOS 发现、操作结果协调、checkpoint 和锁存持久化。[归属方关卡记录](https://github.com/Island-Team/Island-egia/blob/master/docs/egia/develop/build-and-run.md)列出精确测试命令和保留日志。 |
| 目录编辑器、连线检查器、浮动面板与逐绑定控制 | IsosUI [U01 #40](https://github.com/Island-Team/IsosUI/pull/40)、[U02 #41](https://github.com/Island-Team/IsosUI/pull/41)、[U04 #42](https://github.com/Island-Team/IsosUI/pull/42)、[U03 #43](https://github.com/Island-Team/IsosUI/pull/43)、[选定目录 #44](https://github.com/Island-Team/IsosUI/pull/44)及[刷新 #45](https://github.com/Island-Team/IsosUI/pull/45)覆盖元数据表单、不可用选择、连线类型／单位及指向连线的错误、键盘与焦点、窄屏与布局持久化、本次运行日志游标、过期修订、每个活动底盘／机械臂绑定一个控制、类型化结果跟进、bridge 故障及 Stop。[UI 归属方记录](https://github.com/Island-Team/IsosUI/blob/master/docs_cn/isosui/design_cn.md)列出精确命令和测试数量。 |
| 固定版本的安装后跨归属方操作与恢复 | 历史 [M4 受影响边界重放](m4-affected-replay_cn.md)通过 `M4_AFFECTED_VERIFIED_PASS`：两条路由证据边、166 条归属方事件、一条对应 EOS gRPC journal 的受监督任务、一条经公开 bridge 完成的独立浏览器命令，以及选中并确认的 Stop。反例归档通过旧 ID 拒绝、未解决安全锁存／重置拒绝及 EGIA SIGKILL 后的依赖停止／回收。后续 [final-17 选定主题重放](../../docs_cn/eos/maintenance/q03-selected-matrix-evidence_cn.md#final-17-选定主题提交结果2026-11-03)和 [final-5 合并提交重放](q04-merged-qualification_cn.md#第五次尝试merged-home-final-5完整归档已验证)通过六案例离线验证，覆盖注入故障／Restart、安全 Stop、可见且关联的浏览器完成、渲染 Vision、无效软件包／连线／编辑器路径及显式迁移。各次选定文件与封印保持独立。 |

历史 M4 IsOS 固定版本不支持同一次运行内替换 EGIA 子进程。EGIA #98 证明归属方稳定运行内的进程恢复；final-17 与 final-5 证明选定 IsOS 故障及新运行 Restart，不宣称同一次运行内替换。final-5 的托管 Origin 检查使用本机模拟，并非公网 HTTPS。后续安装后 EGIA wheel/CMake 托管进程的 [Q03 证据](../../docs_cn/eos/maintenance/q03-evidence_cn.md#补充验收安装后的-egia-托管进程2026-11-04)是额外归属方验收，不改写历史 M4 输入。M5 公开快照、最终发布评审及标签仍待完成。
