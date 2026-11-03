# Q01 集成首次使用验证设计

[English](../../docs/island/q01-first-use-design.md) · [0.x 待办](../../docs/island/implementation-todo.md) · [里程碑流程](implementation-milestones_cn.md) · [限定范围的 Restart 证据](q01-restart_cn.md)

**设计状态：**已独立评审，并在下列五个所选所有者提交上完成 Q01 验证。本文定义现有所有者接口之间的验证流程和证据条件，不增加公开线协议字段或命令权限。

## 所有者流程和固定输入

IsOS 拥有已保存图的原始字节、修订、已验证包收据、所选节点选项、生成配置、Apply、运行标识、授权、日志范围、Restart 和 Stop。EOS 读取已应用图，拥有活动修订、实时健康、Dispatcher 准入、日志和直接管理的 MuJoCo 子进程。EGIA 拥有任务状态、提案、安全决策、操作检查点及结果。IsosUI 显示所有者状态，并针对活动绑定发送一次经授权的 `primitive_typed` 请求。图边仅声明绑定，不执行浏览器命令。EOS 边界遵循现有 [gRPC](../eos/interfaces/grpc_cn.md)、[模拟器停止](../eos/interfaces/simulator-stop_cn.md)及[浏览器桥](../eos/interfaces/external/http_cn.md)契约；IsOS 的 `docs/isos/interfaces/{package-verification,mujoco-catalog,node-options,local-api,log-cursors,dependent-stop,control-grants}.md` 和 EGIA 的 `docs/egia/interfaces/{pipeline,safety-reset}.md` 分别定义其所有者边界。

启动前记录 EOS、IsOS、EGIA、IsosUI、MuJoCo 的准确已合并源码提交，以及所选二进制、UI 分发文件、MuJoCo 归档/发布清单/类型侧车、夹具及重放源码的 SHA-256，并记录构建环境与可复现命令。第一份实现候选从 EOS `bb3d64536d540967064e0de7048b5d2517082dd6` 和已评审的[限定范围 Restart 证据](q01-restart_cn.md)开始。正式重放前重新核对所有者远端集成分支；从所选源码构建到稳定路径，要求所有者源码检出区干净，并记录实际二进制哈希。旧版本证据单独标记，不能代替当前版本的实时断言。

主操作流程在一个全新的私有 `ISOS_HOME` 和同一个 IsOS manager 会话中运行至最终 Stop。旧版、未来版本和不安全进程用独立的一次性子主目录，不能改变主图或运行历史。验证脚本不得绕过 IsOS 修改已保存图，不得清除 EOS/EGIA 安全锁存，不得把授权内容写进日志，也不得把启动日志行当作就绪状态。

R06/A03 包比较须在安装视图中保留所选资产非空的 `required_libraries` 和 `tested_ubuntu`。完整相等比较前仅归一化契约规定的空字段省略：所选发布清单未声明时安装视图可有 `data_types: []`；所选资产声明空数组时安装视图可省略该字段。收据 SHA 与证据清单分别检查原始字节完整性；重新封存后对非空库、Ubuntu 支持或数据类型的篡改仍须被语义比较拒绝。

## 首次使用主流程

| 步骤 | 所有者动作与必需观察 |
| --- | --- |
| 1. 安装 | 初始化空主目录；捕获图原始字节/修订、活动修订和子进程集合。执行 `isos pkg install --manifest <release.json> --asset <archive> --allow-unapproved`。要求 schema-2 收据精确指向 `island/mujoco-sim@0.1.1`，并包含 `source_kind:local_candidate`、源码提交、归档/发布清单/类型侧车 SHA-256 和规范化安装载荷摘要。安装本身不改变图字节、活动修订、运行 ID 或子进程。归档收据和安装载荷供离线验证。 |
| 2. 选择 | `POST /node-sets/preview` 把 MuJoCo 节点集合组成草稿，但不保存。`POST /node-options` 检查草稿，返回 `graph_revision:null`；所选场景、摄像头、底盘、机械臂和模拟器设备 ID 必须在准确包版本下可用，同时至少保留一项明确不可用的选项。套接字是所有者专用 Unix 路径，不是选项 ID；在预览、Apply 和 Run 时检查其路径及私有所有权。加入七个 EGIA 阶段、包贡献的进程类型、通用 EOS 设备、固定安全路由及两条合格的证据路由边。保留预览字节、选项行、所选 ID 和目录元数据。 |
| 3. 失败后应用 | 启动前读取无过滤的 manager 会话 `GET /logs?view=current&source=all`，保留待定范围。先 Validate 一份无效边草稿，再使用正确修订真正执行无效 `PUT /node-graph`；要求 HTTP 422、精确诊断码/路径、图/活动状态/子进程均不改变，首个锚点为同一诊断的 `deployment_preflight_failed`。修正草稿后 Validate，并按修订 Apply。要求返回修订等于图原始字节 SHA-256，活动修订仍为空，所选包和生成配置/EGIA 覆盖层绑定同一修订。再次读取 `GET /node-options`，要求 `graph_revision` 等于已保存图修订。 |
| 4. 启动与控制 | 通过 IsOS 启动；要求 EOS、MuJoCo、桥、配置服务和 EGIA 的所有者就绪，同一运行 ID，一致的保存/活动/覆盖层修订，以及完整 EOS gRPC 发现。执行一次受管 EGIA 任务，顺序为阶段 → 提案 → 准入 → 提交 → 终态，只有一个操作 ID。在 IsosUI 检查所选节点卡，并对一个活动 primitive 绑定实际点击一次；保留 DOM/绑定/结果和不同的浏览器命令 ID。每个 ID 必须精确对应一条类型化、已准入、终态的 EOS journal 记录，运行、修订、入口、角色、目标和参数均匹配。图边或 UI 本地状态不能作为执行证据。 |
| 4a. 摄像头证据 | 在准确所选源码版本下，于主部署或独立的一次性所选部署执行有界摄像头用例：一帧 MuJoCo 图像携带源节点、源序号、采集代次、帧 ID 和保存图修订，经 EOS 摄像头路由与 gate，出现在 Vision 只读观察中，并以相同来源到达 EGIA 只读证据输入。注入一个过期代次和一个错误修订，要求明确拒绝，两个消费者均不更新。为合格图像路由设置正数 `frequency_hz` 上限，受控过量产生帧，要求 route-status 的 `cap_wait_count` 以及可见的 `replaced_count`/`gap_count` 增加，交付代次仍按序且有界。图像路由只保留最新帧，其损失是替换，不是 FIFO `overflow_count`。另在同一所选版本执行合成 state/event FIFO 用例，超过 64 条/1 MiB 边界，观察 `overflow_count`/`gap_count`，不影响执行器路由。归档不含图像字节的摄像头/gate/Vision/EGIA 原始元数据和路由计数，离线验证源、修订、代次及拒绝/计数顺序。若所有者没有只读观察接口，先实现并评审该所有者切片，再关闭 Q01。 |
| 5. 日志 | 先捕获无过滤的首个就绪/失败锚点，再分别给任务和浏览器 ID 使用独立的关联过滤 cursor，有限分页直到匹配结果。把无过滤 cursor 用于不同过滤条件必须得到 `log_cursor_filter_mismatch`。保留原始 JSONL、响应页、不透明 cursor、范围 ID 和轮转证据。实时状态以 status 为准。 |
| 6. 故障与重新启动 | 注入[限定范围 Restart 证据](q01-restart_cn.md)中的有限次数 HTTP-200 无效控制器回复；要求所有者解析尝试达到配置上限、任务失败，且没有新增 EOS Submit 或移动。只解除模型故障。在同一个 manager 会话中显式执行 IsOS Restart；必须先确认旧 Stop、回收旧 PID、轮换运行/授权、保持保存修订，并建立新的活动修订、完整 EOS 发现及规划器可见的 EGIA 发现。新授权下旧操作不可见（`NOT_FOUND`），旧授权被拒绝，旧移动不重放。新任务使用新 ID，产生一条关联 EOS 结果。旧的无过滤日志 cursor 返回 `log_run_changed`；新运行有自己的首个锚点。 |
| 7. 停止 | 执行受管 robot Stop 和 IsOS Stop。要求所有者确认终态、EOS 所选 Stop 收据条件、执行器静止后终止模拟器、回收子进程和清理套接字。强制、丢失收据或未知 Stop 仅属负例，不能通过此行。 |

## 迁移与负例夹具

冻结 IsOS `tests/fixtures/graph_compat/{legacy_1_0_0,legacy_1_0_0_whitespace,future_2_0_0,unknown_type_1_0_0,unsafe_component_1_0_0,legacy_unmapped_1_0_0,migration_expected_0_1_0,migration_mapping}.json` 的原始字节及 SHA-256。每个负例使用独立的一次性子主目录。GET 必须保留源字节；未来版本在 `/version` 返回 `unsupported_graph_version`，不安全进程在精确的 `/node_type` 返回 `unsafe_process_node`，未知和未映射类型遵循各自所有者诊断。Validate、Apply、Run 均失败且不写入、不启动子进程。已知安全旧图的 GET 只显示单独的只读投影；迁移预览不写入，过期迁移提交返回 409，使用准确源修订与提议修订的显式提交写入规范 `0.1.0` 字节和已评审类型映射。比较全部未变字段及空白字符不同的源修订。现有旧图夹具没有 MuJoCo 包 pin，迁移必须保留空 pin 集，不能虚构。

以下 Q01 最低矩阵绑定表对所有剩余负例都选择**新的所选版本用例**。旧的 M1–M4 归档只作实现参考，均不能代替 Q01 证据，因此不声称旧清单或源码差异已冻结。每个用例都在 Q01 运行归档中保留仅含必要元数据的原始响应、准确所有者源码/二进制版本和校验清单。缺少任何用例都不能关闭 Q01。

| 用例 | 新所选版本证据和拒绝条件 |
| --- | --- |
| 摄像头/路由 | 步骤 4a 证明类型化来源 → gate → Vision/EGIA 来源、过期/错误修订拒绝、实时图像频率上限/替换/缺口，以及合成 state/event FIFO 溢出/缺口。 |
| G06/R03 | 一次性当前版本图/API 用例在准确路径拒绝固定安全 QoS 和旁路，保存字节不变。Dispatcher 用例拒绝同 ID 冲突身份、到期期限和释放后未知结果，不能二次调用适配器；归档 journal 身份及停止隔离。 |
| P01/P03/U01 | 篡改的安装归档/收据以及不可用场景/设备选项在 Apply 或启动前被拒绝；过期图修订的 Apply 返回 409。可见编辑器保留不可用原因，不启用无效选择。 |
| P07/U03 | 受控日志轮转保留首个失败/就绪锚点；过滤不匹配和篡改 cursor 被拒绝，脱敏输出没有授权或模型提示词字节，manager 重启按所有者 cursor 契约保留旧范围或明确拒绝。主流程同一 manager 的 Restart 还要求 `log_run_changed`。 |
| R06/A03 | 一次性所选子进程故障和丢失/强制 Stop 报告未知终态，回收依赖进程并阻止新 Run/Restart；归档的检查点/锁存仍未解决，移动不重放。主正例仍需确认的 Stop。 |

所选版本用例归档必须指明准确执行的所有者测试或实时夹具及其断言；单元用例不能替代跨所有者摄像头来源或主 UI/任务流程。若所有者测试使用模拟器或受控桩，记录环境，其结论范围不得扩大为未观察到的物理实时行为。

## 归档与评审关卡

保留的 `logs/test/<run-id>/` 归档包含原始命令/标准输出/标准错误、环境和准确源码/二进制哈希、原始资源归档和安装载荷/收据/类型侧车、图原始字节/生成覆盖层、所选目录/选项、已服务 UI 资源和 DOM/结果、IsOS 事件 JSONL 与 cursor 页、EGIA 所有者事件/检查点、EOS `.rec` 和 journal SHA 清单、进程状态/回收观察，以及迁移源/预览/保存字节。不归档授权原文和模型提示词。校验清单覆盖所有不可变文件且不自引用。离线校验器只读归档：核对完整文件集合和哈希、所有者标识与顺序、保存/活动/覆盖层修订、命令/结果/journal 关联、首锚点与 cursor 范围、Stop/回收证据和迁移诊断；不读临时主目录、实时 `/proc`、源码检出区或网络。篡改副本至少应在包/归档字节、图/修订、UI 资源、journal 记录、日志范围/cursor 及迁移字节六类中被拒绝。

实现前先取得本所有者流程及冻结夹具的独立设计评审。实现分支要有有意义的聚焦及跨所有者负例测试，然后运行 EOS 格式、严格 Clippy、串行工作区测试及构建，IsOS Cargo 检查，EGIA Ruff/pytest/pyright/导入检查，IsosUI 测试与两种构建，以及受影响时的 MuJoCo CMake/CTest。完整命令和结果存入同一运行 ID，取得独立实现评审；在完整所选版本重放和旧证据绑定通过前保持 Q01 未勾选。Q02 旧调用者清理及 Q03/Q04 发布证据仍是后续关卡。

## 实现重放状态（2026-11-02）

本地 `logs/test/20261102-q01-full/` 归档在 `remote-pins.json` 中记录准确的远端集成提交。`migration-run.log` 和 `migration/evidence.sha256.json` 覆盖所选 IsOS 迁移夹具，并通过 `python3 tests/verify_q01_migration.py logs/test/20261102-q01-full/migration`。`package-negative.log` 记录所选 MuJoCo 归档篡改拒绝。`cargo-fmt.log`、`cargo-fmt-check.log` 和 `python-compile.log` 记录重放代码的格式与 Python 语法检查通过。

`live-11/` 是**未完成**的全新主目录重放。它记录真实包安装、所选选项、无效 Apply 和首个启动前锚点、有效 Apply、受监管任务、可见 IsosUI `base.velocity` 点击及 EOS 终态 journal 记录、所有者日志查询、受限控制器故障，以及同一 manager 的显式 Restart、旧运行 Stop/回收、授权轮换和 `log_run_changed`。所选 IsosUI 源码按本次运行的 IsOS 和 bridge 端口重新构建；准确命令与输出在 `live-11/q01-ui-rebuild.json` 和 `live-11/ui-rebuild.log`。Restart 后所选 EGIA 进程重新发现四个 EOS primitive，但供 planner 查看公开目录在就绪窗口内返回零条目。随后 harness 会话因来源未观察到的 SIGTERM 退出，未完成新任务和最终确认 Stop。`live-11/abort-diagnostic.json` 及其中列出的原始文件保留未完成边界；校验器必须拒绝此归档。

浏览器命令有 EOS 终态 journal 记录，但其关联筛选后的统一日志页为空。`live-11/q01-log-correlation-gaps.json` 标明此所有者事件缺口。摄像头到 EGIA 的只读来源证据、剩余所选版本负例矩阵和完整所有者质量关卡也尚未完成。不能依据 `live-11` 标记 Q01 完成。

`live-13/` 是当前所选版本的主流程重放。EOS 可执行文件由 `bb3d6453` 的 crate 树构建（`q01-eos-build.json`）；IsosUI 资源按本次 API 端口重建，并核对实际服务的文件。全新主目录的安装、无效 Apply 与首次失败锚点、有效 Apply、受监管任务、可见浏览器命令、有限模型故障、同一 manager 显式 Restart、新任务、确认 Stop 和子进程回收均已通过。`live-13-verify-main.log` 记录仅用归档的主流程校验通过；`live-13-tamper.log` 记录五种篡改副本被拒绝。完整校验仍因一个真实缺口失败：浏览器命令 `ui-68d79f43-d523-431c-a62c-7178b587c588` 已在 UI 完成并有匹配的 EOS 终态 journal 记录，但 IsOS 统一日志没有对应关联事件。`live-13-verify-full.log` 保留拒绝结果。所选版本的摄像头所有者接口和其余负例矩阵完成前，Q01 仍保持开放。

独立评审后的 harness 要求在准确的所选 IsOS 源码检出区构建可执行文件，并归档构建日志、源码树及锁文件标识、二进制哈希和安装前后图的原始字节。失败收尾会先请求有限时长的所有者 Stop，再结束 CLI 父进程，并记录已观察到的子进程 PID。离线主流程校验从归档的筛选日志页推导关联缺口，并强制要求任务事件；完整校验对每个矩阵案例调用语义校验器。目前只有迁移案例有该校验器，因此自行声明 `passed` 的矩阵不能关闭 Q01。这些更严格的条件适用于新重放；此前的 `live-13` 结果仍是采用当时归档 harness 的历史证据。EGIA 启动程序来自固定源码检出区且记录了字节哈希，但尚无独立的 EGIA 包构建证明。校验和清单证明保留文件的一致性与篡改检测；人工浏览器观察由 EOS 结果和 journal 记录佐证，离线校验器无法单独重现点击。

修复检查输出保存在 `logs/test/20261102-q01-harness-repair/`：在 `/tmp/isos-m4-2bdf` 运行 `/home/ashex/.cargo/bin/cargo build --locked --bin isos` 通过；`python3 tests/test_q01_harness_guards.py` 的三个守卫测试通过；`python3 tests/verify_q01_migration.py logs/test/20261102-q01-full/migration`、`python3 -m py_compile tests/{verify_q01,verify_q01_cases,verify_q01_migration,m4_affected_replay,test_q01_harness_guards}.py`、`cargo fmt --all -- --check` 及 `git diff --check` 均通过。更严格的校验器按预期因缺少 IsOS 构建收据而拒绝旧 `live-13` 归档。其余所有者接口落地后，还需新一轮含浏览器实际操作的所选版本重放。

所选版本浏览器边界重放位于 `logs/test/20261103-q01-browser-boundary/live-2/`，使用 EOS `64216657`、IsOS `25e9a8f`、EGIA `0c0c767` 和 IsosUI `6afd5bd`。可见工作台对活动 `base.velocity` binding 提交命令 `ui-6d1b7c54-77a6-4a36-a4d7-bd8fb4b6ae27`，参数为 X 方向 `0.1 m/s`、租期 250 毫秒。UI 与 EOS 结果后续查询均为 `Completed`；EOS 终态 journal 记录的命令、binding、运行和修订一致。IsOS 当前运行筛选日志包含该命令的 `submit` 和 `followup` 事件，关联缺口为空。harness 完成同一 manager Restart、确认 Stop 和子进程回收。归档专用校验器 `tests/verify_q01_browser_boundary.py` 在指定三个源码提交时通过；两个篡改后的结果/日志副本被拒绝。第一次尝试 `live-1/` 因系统 Python 缺少 `websockets` 在任务前失败，保留为失败的环境运行。这是限定边界证据，不能关闭 Q01；主校验器仍固定较旧提交，且其他矩阵案例和完整所有者质量关卡尚未完成。

经独立复审的 G06/R03 用例位于 `logs/test/20261103-q01-g06-r03/review-repair-final/`，在 EOS `64216657` 和 IsOS `ac984bc` 上通过 `python3 tests/verify_q01_g06_r03.py <archive>`。归档保留所选源码构建收据、两份拒绝的连线草稿、未变保存图和无子进程状态、原始 Dispatcher journal、期限/身份/未知结果的受控测试及 Stop 隔离。四种重新签署校验清单的语义篡改被拒绝。独立复审见 `logs/test/20261103-q01-g06-r03/review-rereview.md`。本用例已接入完整校验器；任一所选提交变化均需重放。

经独立复审的 P07/U03 用例位于 `logs/test/20261103-q01-p07-u03/live-8/`，在 IsOS `ac984bc` 和 IsosUI `91c0cc6` 上通过 `python3 tests/verify_q01_p07_u03.py <archive>`。保留的 cursor 状态、准确分段身份和轮转原始文件证明首个启动前失败锚点在其分段过期后仍可查找；被篡改或过滤不匹配的 cursor 被拒绝，manager 重启更换范围，挂载 UI 测试证明 Logs 关闭后仍能使用常驻 Stop。五种重新签署校验清单的语义篡改被拒绝。独立复审见 `logs/test/20261103-q01-p07-u03/implementation-rereview.md`。本限定用例已接入完整校验器；主部署运行的 `log_run_changed` 与其他 Q01 行仍需单独验证。

所选摄像头重放位于 `logs/test/20261103-q01-camera/live-10/`，在 EOS `64216657`、IsOS `ac984bc`、EGIA `8ed5cdf` 和 MuJoCo `4e6b544` 上通过归档校验器。归档保留准确匹配的来源 → gate → 浏览器/EGIA 帧来源、所选构建收据、图像频率上限/替换/缺口计数、所选源码的合成 FIFO 溢出、实时错误修订和旧流 cursor 拒绝及两消费者稳定状态，以及确认 Stop/撤销。六种重新签署校验清单的语义篡改被拒绝。位于 `logs/test/20261103-q01-camera/implementation-rereview.md` 的独立复审认为此用例仍**未关闭**：实时旧流 `cursor_stale` 拒绝并未向 EGIA 注入同一流的重复 generation。所选 EGIA 受控桩会拒绝 `generation_stale`，但 Q01 步骤 4a 还要求所选部署中的消费者观察。需先为严格限定的 EGIA 所有者测试接口完成设计、评审、实现和新重放，才能在总校验器中启用摄像头用例。

历史 P01/P03/U01 归档位于 `logs/test/20261103-q01-p01-p03-u01/live-7/`，在 IsOS `ac984bc`、MuJoCo `4e6b544` 和 IsosUI `91c0cc6` 上通过离线校验。它保留可重建的源码树与新构建的 IsOS、单字节载荷和收据摘要篡改前后的完整已安装包树、不可用场景/设备的 Validate 与 Apply 诊断、过期 Apply，以及可见编辑器实际消费的 node-options POST；编辑器显示不可用的禁用选项和禁用的 Save and Run。包括篡改浏览器所消费响应中 `available` 标志在内的七种重新签署语义篡改均被拒绝。位于 `logs/test/20261103-q01-p01-p03-u01/implementation-final-review.md` 的独立最终复审批准此历史范围。随后 IsOS master 前进到 `00c1d7a`；此用例接入完整矩阵前仍需按新所选提交重放并评审。

当前提交的 P01/P03/U01 重放位于 `logs/test/20261103-q01-p01-p03-u01/live-8/`，在 IsOS `00c1d7a`、MuJoCo `4e6b544` 和 IsosUI `91c0cc6` 上通过同一离线校验器及七种语义篡改负例。独立的当前提交评审见 `logs/test/20261103-q01-p01-p03-u01/current-pin-implementation-review.md`。这关闭了这些所有者提交上的限定包/目录负例，其精确提交语义校验器已接入完整矩阵；单次所选提交的主运行仍未完成。

IsOS `00c1d7a` 上的 G06/R03 和 P07/U03 重放分别保留在 `logs/test/20261103-q01-g06-r03/current-00c-2/` 与 `logs/test/20261103-q01-p07-u03/live-9/`，两个新归档均通过仅依赖归档的语义校验。G06/R03 首次尝试 `current-00c/` 因把 IsosUI 的 `dist` 父目录传给 IsOS 而在就绪前失败；成功重放改用 `dist/local`。位于 `/tmp/q01-selected-pin-rerun-review.md` 的独立所选提交评审批准了这两个限定所有者用例，并要求当前提交的语义篡改记录。现在 `tamper-current-00c-2.log` 中三种 G06/R03 篡改及 `tamper-live-9.log` 中五种 P07/U03 篡改均被拒绝；后续独立复审仍未完成。完整 Q01 校验器现于 CLI 和库调用均要求从外部提供五个源码提交标识（含 MuJoCo 包源码），避免重新签署的归档自行选定所有者身份。主流程重放也可显式指定 IsosUI 提交；新的主流程运行以及摄像头和 R06 负例仍未完成。

迁移用例正在 IsOS `00c1d7a` 上重放，输出位于 `logs/test/20261103-q01-migration/`。归档现保留所选 IsOS 源码提交、源码树和二进制摘要；矩阵校验器还将该摘要绑定到主流程所选的 IsOS 二进制。较旧的迁移归档仍只作历史证据。独立评审和最终矩阵组装仍未完成。

主流程所选提交重放现于 manager 启动前在私有主目录写入显式产品配置：EGIA 的 `share` 条目依据准确的所选 EOS 检出区生成，归档原始与选定 TOML 字节及摘要，并在离线校验中核对路径的词法映射。这样，位于分离检出区的 EGIA 进程不会误读主要 EOS 兄弟目录的资源。新的完整运行还须证明使用该配置成功启动；旧主流程归档没有此项证明。

所选摄像头重放 `logs/test/20261103-q01-camera/live-13/` 在 EOS `64216657`、IsOS `00c1d7a`、EGIA `694c58c` 与 MuJoCo `4e6b544` 上通过仅依赖归档的校验。归档包含实时私有 EGIA 同流重复帧重放，返回 `generation_stale`：已接受帧的身份和计数不变，`rejected_count` 及原因计数各增加一。错误修订和旧流的 hook 拒绝单独记录；原有 EOS 错误修订/旧流、图像限速/替换/缺口、合成 FIFO 与 Stop 用例仍在归档中。两种重新签署的语义篡改副本被拒绝。`live-11`（系统 Python 缺少 `websockets`）和 `live-12`（受控探针固定了合并前的 EGIA 提交）保留为失败尝试。独立实现评审正在进行；此用例尚未接入完整矩阵。

## 所选五所有者 Q01 验证（2026-11-03）

所选集成提交为 EOS `642166576a2cddfd97cd703e5bd9e53567745515`、IsOS `00c1d7aa184de8b969e6e40472bf9b1fa55032ff`、EGIA `d3868d2763c35d9f6bfed80d75edb7a1844ef54e`、IsosUI `920995788d8e29dd6ab594d31278033bb791cfeb`、MuJoCo `4e6b5449ccd25be53ad95b13baa2c89f7ed91e10`。全新主目录主流程归档位于 `logs/test/20261103-q01-full-current/live-3/`，包含六个已封存用例：摄像头来源及路由、G06/R03 边与准入负例、P01/P03/U01 包与选项及可见编辑器负例、P07/U03 轮转日志游标、R06/A03 未确认 Stop 与回收及恢复、显式迁移。实际工作台点击与 EOS 终态 `Completed` 使用同一命令 ID；主流程保留故障、Restart、新任务与最终 Stop 证据。

使用英文页所列带五个外部提交参数的 `python3 tests/verify_q01.py` 命令重现离线验证。`logs/test/20261103-q01-full-current/matrix-full-verify-6.log` 以 `Q01_FULL_VERIFIED` 结束；归档无符号链接，验证器拒绝非普通文件。错误外部提交或缺失矩阵用例均被拒绝。`/tmp/q01-full-matrix-independent-review.md` 的独立评审批准 Q01 范围。同一忽略日志目录内，定向脚本测试、EOS 格式检查、Clippy 和串行工作区测试通过。并行工作区测试有四个后端子进程测试失败；串行重跑通过。

主流程 shell 包装器写入限定通过与最终 Stop 证据后以 143 退出，因此验证依据是离线门槛和原始所有者记录，不能声称包装器零退出。名为 `receipt-loss` 的 R06 用例证明 EOS 子进程退出后的未确认 Stop，未证明解析器专属的 `eos_receipt_missing` 代码。Q02 C++ 最后调用者清理、Q03 发布门槛与基准、Q04 已评审合并与标签仍待完成。
