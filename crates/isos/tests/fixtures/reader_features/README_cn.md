# P05 读取器特性夹具

[IsOS 归属契约](../../../../../docs_cn/isos/interfaces/packages/verification_cn.md)定义目标行为。`cases.json` 冻结 45 个实现用例。每项都有稳定 `id`、操作、命名变更和 `expected` 结果。`reader_preflight` 是隔离的 P05 身份检查；缺少 R02 网关就绪时，`admit_reader_only` 绝不表示公开 Run 成功。只有 EGIA setup 声明精确图／类型 ID；EGIA runtime 只声明特性，不含图／类型数组。拒绝用例要求精确代码、阶段、图 JSON 指针、所选程序 ID、适用时请求的特性、保存字节不变及零次停止／启动调用。Restart 失败还要求活动子进程 PID 不变。基线图使用已有 `0.1.0` 连线夹具；实现根据归属注册表构造命名的起始图、MuJoCo 和旧版变体。该文件是声明式设计证据，不表示 P05 运行时已经通过。

正例覆盖精确 EOS 起始图、安全的仅标量旧版读取、不读取图的 HTTP bridge、`0.1.0` 外壳中的 EGIA 已注册旧类型，在 setup／启动时通过 `--node-types` 加载的已验证 MuJoCo 包贡献、不带 sidecar 的 EOS 能力探测，以及带 runtime 特性的旧版 MuJoCo 子进程。负例覆盖 G01／G03 诊断优先级、所选数组缺少已知图或已注册类型、错误／部分数组回退、条件特性（包括无 Vision 节点的启用 bridge）、P05 之前拒绝旧版 backend、精确进程路径与程序身份、包 pin／收据变化，Apply 后所选程序变化，以及 EGIA runtime 无数组／无引导和解释器／模块来源边界。探测相邻程序不能替代实际所选子进程缺少的特性。升级用例要求精确已验证发行证据，并禁止 `version check` 顺带安装或选择。

实现应使用干净 `ISOS_HOME`、固定摘要的所选 setup/runtime 探测桩，以及包用例所需的已验证 `island/mujoco-sim@0.1.1` sidecar。Run/Restart 拒绝从活动 EOS-only 部署开始，断言活动修订及 PID 集合不变。既要测试 `isos version check --json`，也要测试 HTTP／CLI 准入。完整输出保存于忽略的 `logs/test/<run-id>/`；P05 关闭前应按归属契约的命令与断言检查。用 `python3 tests/fixtures/reader_features/check_cases.py` 校验夹具。
