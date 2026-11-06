# 0.x UI 目标实现 TODO

本记录对应 [IsosUI 0.1.0 实现](../island/retired-repository-references_cn.md#ref-edd683471a30) 和共享的 [M4 智能体与工作台关卡](https://github.com/Island-Team/Island-eos/milestone/4)。IsosUI 所有者切片 U01–U04 已合并到 `master`；跨所有者 M4 退出验证与 Q04 发布决定已在获批准的发布资产 run-7 中分别完成。首次使用时先显示所属方状态，再显示本次运行的因果日志。

[English](../../docs/isosui/todo.md) · [设计](design_cn.md)

1. **已完成——图与目录读取。** 多版本读取保留兼容字段及旧来源引用；所选目录与保存修订限制编辑和 Invoke。见 [U01 #40](../island/retired-repository-references_cn.md#ref-3e217f90bcc6) 和[所选目录 #44](../island/retired-repository-references_cn.md#ref-73fa37dd8626)。
2. **已完成——U01 表单与选择器。** 元数据表单及可搜索场景／设备选项保留不可用 ID，并阻止无效 Apply。模型权重执行不属于 0.x。见 [U01 #40](../island/retired-repository-references_cn.md#ref-3e217f90bcc6)。
3. **已完成——U02 连线。** 精确 IsOS 诊断、已路由优先级／频率编辑、旧图默认值，以及仅设计／固定路由的无障碍标签由 [U02 #41](../island/retired-repository-references_cn.md#ref-1cc1289a9d7d) 覆盖。
4. **已完成——U04 节点信息卡。** 悬停、焦点和触摸展示类型与实例描述、端口、单位及所属方报告的健康，不妨碍图编辑。见 [U04 #42](../island/retired-repository-references_cn.md#ref-2055acd51118)。
5. **已完成——U03/U04 工作台。** 独立控制、监视、Vision 和日志面板保存有界布局及 Stop 权限；每个受支持活动原语绑定有一个图解析目标、类型化 `primitive_typed` 控件和关联结果跟进。图连线不执行命令。见 [U04 #42](../island/retired-repository-references_cn.md#ref-2055acd51118)、[U03 #43](../island/retired-repository-references_cn.md#ref-0ead76ead8f5) 和[刷新 #45](../island/retired-repository-references_cn.md#ref-61b682643d4b)。
6. **已完成——本次运行日志。** Home 使用 IsOS JSONL 游标、固定首事件锚点、缺口提示及来源筛选，并将命令结果与日志区分。见 [P07 UI 契约](interfaces/log-cursors_cn.md)。
7. **所有者检查已记录。** 中英文首次使用、无障碍交互、Node 22/npm 10 测试及本地／托管构建见 [Q03 所有者证据](q03-evidence_cn.md)。封存的 Q03 final-17 选定回放和 Q04 final-5 合并后回放通过各自精确输入验证。Final-5 在回环地址模拟托管 Origin；这不证明真实公网 HTTPS 首次使用。后续 IsosUI 标签与 Island-web 部署另见[Q04 发布证据](interfaces/releases_cn.md)。

[历史 M4 受影响边界回放](https://github.com/Island-Team/Island-eos/blob/refactor%28v2%29/runtime-main/docs_cn/island/m4-affected-replay_cn.md)验证了一次受监管任务、浏览器命令及选定 Stop，但该 IsOS 固定版本无法在同一运行中替换 EGIA。[后续 Q03 final-17 与 Q04 final-5 证据](q03-evidence_cn.md)分别固定输入，不能改变该历史结果。后续[已批准的发布资产 run-7](https://github.com/Island-Team/Island-eos/blob/refactor%28v2%29/runtime-main/docs/island/q04-merged-qualification.md)记录五个已发布标签，完成既定范围的 Q04/M5 发布关卡。IsosUI `v0.1.0` 已发布，托管归档已由 Island-web 固定；真实公网 HTTPS 首次使用仍需单独验证。物理 ROS／厂商设备和 EGIA tensor 节点属于后续工作。

## M5 Q03 验证——已记录

[Q03 设计](q03-qualification_cn.md)与[所有者证据](q03-evidence_cn.md)追踪选定 IsosUI 源码、本地／托管浏览器基准及封存的跨所有者 final-17 归档。静态检查和隔离桩浏览器路径与洁净主目录回放分别记录。另行选定的 Q04 final-5 合并后归档也通过独立审查。IsosUI `v0.1.0` 发布与 Island-web 部署已记入发布契约。[已批准的发布资产 run-7](https://github.com/Island-Team/Island-eos/blob/refactor%28v2%29/runtime-main/docs/island/q04-merged-qualification.md)完成既定范围的 Q04/M5 跨所有者关卡；公网 HTTPS 首次使用仍待验证。
