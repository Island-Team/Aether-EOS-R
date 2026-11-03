# 读取器预检设计

[English](../../../../docs/eos/design/runtime/reader-features.md) · [归属方契约](../../interfaces/reader-features_cn.md) · [P05 任务](../../../../docs/island/implementation-todo.md)

**EOS 探针部分已实现；IsOS 准入仍待完成。** EOS 如实声明每个所选二进制文件能读取什么。IsOS 持有对已保存部署的准入决定。[读取器契约](../../interfaces/reader-features_cn.md)定义线字段和精确特性令牌；本文说明使用方式，不创建第二份图清单。

Profile 工具、runtime 和 config server 共享同一个 EOS 内建注册表。它们的 `--capabilities` 实现应从注册表取得精确内建 ID 列表，不能复制一份手写表。Profile 工具的 setup 身份应复用自己的能力常量，使 `setup --json` 不可能与 `--capabilities` 矛盾。软件包贡献类型是另一份已验证输入：IsOS 仅将经过 P01 验证的 sidecar 传给 `setup catalog`，由 profile 工具从这些 sidecar 建立有效注册表。静态探针不声称软件包已经安装、选中或准入。

对每份候选图或已保存图，IsOS 先确定精确的保存字节修订号，验证引用的软件包 pin，并解析其 P04 二进制文件／默认值输入清单。它从实际 EOS 节点、连线和启动参数推导所选 EOS 进程角色及必需特性，直接执行每个选中文件，检查返回的身份、版本数组、精确内建 ID 与必需特性子集。随后使用已验证 sidecar 请 profile 适配器校验 EOS 投影。探针失败或不匹配必须在归属方 Render、生成版本选择、活动子进程停止或新进程启动前拒绝。Run 与 Restart 时，不可变生成版本中的二进制哈希和 P05 观察结果仍须匹配；显式 Apply 才能选取新的生成版本。二进制文件名或产品版本不能替代这些检查。

最旧的已支持 profile 探针仅返回图／类型标量版本。只有图使用精确注册的 `1.0.0` EOS 身份且不要求未声明特性时，才把缺少数组视为已知旧版兼容情况。存在但无效的数组是探针失败。未知 `@v0.1` ID、未来版本与未验证软件包贡献绝不进入该回退。这保留已有旧版读取能力，同时避免版本字符串凭空生成执行器或浏览器路由。

Bridge 是传输子进程：它读取 runtime socket 和可选 Vision 流，不读取图。只要 bridge 子进程被启用，IsOS 就检查其选中程序的 `vision-ws` 令牌，但不要求它返回图／类型数组。Runtime 的 `mujoco-child-v2` 令牌描述现有直系子进程 v2 socket 组合；对 `eos.process.mujoco@v0.1`、已知 `@v1.0`，以及仅在旧版 `eos.process.backend@v1.0` 节点选择已验证的 `island/mujoco-sim@0.1.1` v2 子进程时，均要求此令牌。R05 将另行定义 strict-socket 就绪；此令牌不得扩展为暗示后续保证。

实现测试应比较真实调试版或发行版二进制探针与 setup 身份、全部 18 个精确内建 ID、当前特性数组及按条件添加的 MuJoCo 特性。Stub 探针覆盖格式错误、重复、缺失、未来版本和旧版标量情况。边界集成测试依次替换一个所选二进制文件，测试新图和安全旧图，变动已验证 sidecar 与一个必需特性，并断言拒绝时无子进程启动且活动 PID 不变。完整输出存于 `logs/test/<run-id>/`，记录所选二进制路径和哈希、命令与结果摘要。[冻结夹具](../../../../tests/fixtures/reader_features/README.md)列出归属方用例；公共诊断夹具由 IsOS 持有。

2026-11-01 的设计基线：`python3 scripts/check_doc_links.py --all` 检查 294 个 Markdown 文件，损坏链接为零；`python3 -m json.tool` 接受两个冻结 JSON 文件；`git diff --check` 和 `cargo fmt --all -- --check` 通过。命令与完整输出保存在忽略 Git 的 `logs/test/20261101-p05-eos-design/`。这些检查只验证契约草案，不证明二进制特性已经实现。

独立设计审查发现初稿漏掉两处实际启动边界。修订后的契约和 23 项夹具对两个软件包贡献的 MuJoCo 进程版本及锁定 pin 的旧版 backend 均要求 `mujoco-child-v2`，并对每个已启用 bridge 要求 `vision-ws`。修订后同样通过文档、JSON、diff 和格式检查；完整输出位于忽略 Git 的 `logs/test/20261101-p05-eos-design-review/`。

EOS 实现证据（2026-11-01）：`cargo fmt --all -- --check`、`cargo clippy --workspace --all-targets -- -D warnings`、`cargo test --workspace`（126 项通过，一项原有测试忽略）、`cargo build --workspace`、`python3 tests/reader_features_cli.py`（四项通过）和 `python3 scripts/check_doc_links.py --all`（294 个文件、零损坏链接）均通过。二进制重放将四个真实探针及 setup 身份与冻结夹具比较，检查无输出文件的错误 setup 请求、三种直系 MuJoCo 子进程形式与已启用 bridge 的特性声明，并验证安全 `1.0.0` 与 `0.1.0` EOS 图。完整输出及所选二进制文件的 SHA-256 位于忽略 Git 的 `logs/test/20261101-p05-eos-reader-features/`。按图推导的特性决定与停止活动子进程之前的拒绝重放仍由 IsOS 负责。
