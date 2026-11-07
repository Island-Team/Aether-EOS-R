# R03 IsOS 控制授权交接用例

[English](README.md) · [IsOS 所有者契约](../../../../../docs_cn/isos/interfaces/cores/control-grants_cn.md) · [设计](../../../../../docs_cn/isos/design/server_cn.md)

`cases.json` 固定 IsOS 签发和启动传递的设计矩阵，尚不表示运行时已通过。基线使用干净的 `ISOS_HOME`、显式选中并验证的 schema-2 V2 图生成代、必需的已选 EOS stub，以及可选的已选 EGIA 和 bridge stub。stub 只记录看到的保留环境变量**名称**。EOS R03 测试独立检查验证文件字节、gRPC 元数据、角色/方法范围及 Dispatcher 释放。任何用例都不把真实授权写入源码仓库。

正例覆盖全部角色已选时的三个路径、EOS 加 config 且无 EGIA/bridge 的启动，以及无 bridge 启动、精确文件格式/权限、与保存修订及运行 ID 绑定的验证文件、幂等 Start 和自动子进程重启的重用、相同修订显式 Restart 的轮换，以及替换失败后使用另一个新运行恢复。反例注入不安全文件、错误授权/验证格式、修订变化、停止前候选计划遗漏必需的 EOS、P04 快照缺失、`0.1.0` 的 G03 或 V2 选择缺失、旧 `1.0.0` 图、图作者写入的 `--run-id` 与 `--run-id=...`，以及测试用随机源故障。替换前拒绝必须保持活动 PID、修订与授权包不变，以管理器会话发出 `deployment_preflight_failed` 且不改变旧 P07 当前运行范围。`deployment_starting` 后的已选授权注入故障属于候选运行并有其终结事件，不能保证旧 PID 不变；恢复使用不同的运行。替换失败必须使候选运行终止；旧凭据不能授权恢复。Stop 在回收接收方后撤销授权包。状态、API 错误和 JSONL 事件只能含脱敏代码、运行 ID 与修订，不得包含授权、其摘要或 bearer 文本。

只给所选 profile 或只给验证文件的半托管 EOS 启动必须在监听器绑定前失败。两者皆无时即使存在 `--run-id`，仍为独立只读。R03-I26 在 P05 预检中拒绝缺少精确 `managed-control-grants` 特性的已选旧 EOS 可执行文件，不得先停止子进程或签发授权。

实现测试应将每个 `id` 映射到单元或干净 home 的边界测试，在忽略的 `logs/test/<run-id>/` 记录命令、所选 EOS/IsOS 提交与结果，然后获得独立审查。运行 `python3 tests/fixtures/control_grants/check_cases.py` 验证设计矩阵与双语链接。
