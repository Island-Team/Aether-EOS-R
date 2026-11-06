# Q03 跨仓库验收设计

[English](../../docs/island/q03-qualification.md) · [里程碑](implementation-milestones_cn.md) · [详细 TODO](implementation-todo_cn.md) · [Q01 首次使用](q01-first-use-design_cn.md) · [Q02 最后调用方](../eos/maintenance/q02-evidence_cn.md)

Q03 在 Q01 与 Q02 后验收选定的 0.x 仓库提交，不新增图、执行器、浏览器、模型或软件包契约。IsOS 管理已应用图、软件包和进程；EOS 管理实时准入、日志、子进程和健康；MuJoCo 管理私有模拟器套接字；EGIA 管理智能体状态；IsosUI 呈现归属方报告的状态。每个归属方运行源码门槛和安装边界。跨归属方门槛在 Q02 之后重演 Q01 干净 home 操作路径。旧 Q01 归档可供对照，不能证明后续 EOS 提交已安装并运行。

## 固定输入与验收条件

规划时远程提交为 EOS `refactor(v2)/runtime-main` `dea83f514024dcd577e0284e06d00e5e753ff5a8`、IsOS `master` `00c1d7aa184de8b969e6e40472bf9b1fa55032ff`、EGIA `master` `d3868d2763c35d9f6bfed80d75edb7a1844ef54e`、IsosUI `master` `920995788d8e29dd6ab594d31278033bb791cfeb`、MuJoCo `master` `4e6b5449ccd25be53ad95b13baa2c89f7ed91e10`。这些是起始 pin，不是发布 pin。每个归属方 PR 和选定重演前重新检查远程。记录源码、可执行文件和资源哈希、MuJoCo receipt、选定图字节/修订、活动修订、运行 ID 及命令/结果 ID。陈旧主 checkout 构建不能替代选定远程提交。保留 EOS 和 IsOS 主 checkout 中未跟踪的 `temp_design.md`。

EOS 主 checkout 中选定的 Q01 归档 `logs/test/20261103-q01-full-current/live-3/` 及外部 pin 验证器证明此前干净 home 的六案例矩阵，每例含正反断言。其包装脚本在限定通过和最终 Stop 后以 143 退出；Q03 必须保留这一进程退出区别。Q03 在 Q02 后重新运行安装路径：安装 → 选择 → Validate/Apply → 启动/就绪 → 检查/控制 → 本次运行因果日志 → 注入故障/Restart → 安全 Stop → 显式迁移，包含不支持的图、软件包、边和未知结果路径。新选定 pin 归档须用外部给出的五个准确源码提交通过 `tests/verify_q01.py`。其断言包括同一次运行的保存/活动/生成 overlay 修订一致、日志前由归属方状态确认就绪、一个浏览器命令 ID 关联已准入的 EOS journal 终态记录及本次运行日志、Restart 后不重放旧 grant 或操作，以及模拟器终止和回收前确认 Stop。针对真正服务的选定 `dist/local` 记录浏览器 UI 证据，包括一个活动原语绑定及选定 Vision generation/digest；另以 `/isosui/` 服务 `dist/hosted` 并检查两个编辑器 URL。先 Stop 并回收，再清理。截图、退出状态或旧归档本身不能通过此门槛。

## 归属方门槛

| 归属方 | 源码与安装验收 | 初始基线固定负载 |
| --- | --- | --- |
| EOS `0.2.0` | 根目录 `cargo fmt --all -- --check`、`cargo clippy --workspace --all-targets -- -D warnings`、串行 `cargo test --workspace --all-targets -- --test-threads=1`、release build、契约/文档链接、默认临时前缀安装与 CTest，及选定 IsOS → EOS → MuJoCo → EGIA/UI Q01 重演。保留 Q02 原生 SDK 和可选 C++ 证据。 | 对现有 `cargo run --locked --release -p aether-dispatcher --example bench` 增加经审查的样本与结果计数。保留固定 1,000 次 profile、5,000 次内存 dispatch、100 次持久 dispatch、1,000 次模拟摄像头负载。各负载分别报告；旧 profile 是安全读取基线，不代表新图性能。 |
| IsOS `0.1.0` | 默认及 `--no-default-features` 的 Cargo 格式、Clippy、测试、构建，归属方软件包/选项/预检/日志/停止测试，选定干净 home 安装及准确 receipt，Apply/Run/Restart/Stop，拒绝过时修订并无残存子进程。 | 固定五次干净 home 启动与二十次串行选定摄像头 capture，保存原始单调时钟样本及结果计数。不能将 stub 管理器和真实 MuJoCo 结果合并为一个负载。 |
| MuJoCo `0.1.1` | 对跟踪 C++ 源运行 `clang-format --dry-run --Werror`，CMake 构建/CTest 覆盖严格套接字、容量、停止及 bundle 目录，校验和发布归档及安装的 IsOS/EOS 子进程重演。 | 无图形界面的 pick-place 与 navigation scene 分别测量，seed 42，固定 renderer/摄像头尺寸；预热后每 scene 通过私有套接字串行请求 200 次 base observation 和 20 次 camera capture，重复三个独立进程。私有 benchmark truth 不流向 EOS/EGIA。 |
| EGIA `0.1.0` | Ruff 格式/检查、完整 pytest、pyright、六项导入契约、`islandctl validate`、隔离安装 wheel/探针及选定真实 IsOS/EOS 进程重演，验证七阶段健康。包含不完整发现、过时 Vision generation、结果丢失对账、Restart 和 Stop 负例。 | 七阶段交付 10,000 个有序事件，offered rate 1,000/s，优先级 0、无 cap；再测 30 Hz cap 与第二条优先级 -10 的边。固定本地 Vision stub 流 30 fps、1,000 个递增 generation，每 100 帧插入一个重复。分别公布 offered、admitted 和 delivered 数量。 |
| IsosUI `0.1.0` | `npm ci`、类型检查、`npm test`、`npm run build`，验证本地/托管 bundle 与 manifest，聚焦目录/边/控制/Vision/日志测试及两种语言的选定浏览器首次使用。 | 本地与托管构建分别测 Graph-open/catalog-render 和 Control refresh/result-display，使用图/响应字节大小及延迟固定的 loopback stub：每种情况先预热 100 次，再串行测量 1,000 次，以浏览器单调时间和稳定可见 DOM 为终点。 |

每个归属方在运行前固定 fixture 字节、次数、速率和超时。benchmark runner 只是验收工具，不属于产品运行路径；须有样本计数、失败和超时的聚焦测试。EOS 现有示例须扩展或配套经审查的 runner：保留单次单调时钟样本、检查每个 Result 结果、输出成功/拒绝/错误次数及总时长，并采用声明过的中位数算法与 nearest-rank p99；现有汇总输出本身不足以通过 Q03。没有实测基线前不声明吞吐或延迟目标。每项负载报告成功次数、错误/拒绝/gap、总耗时、吞吐量、单调时钟单次样本的中位数和 nearest-rank p99，并保存所有原始样本。至少重复三次（成本低时 EOS/EGIA 五次），报告每次和聚合方法。记录 CPU 型号/核心数、内存、OS/kernel、编译器/运行时版本、适用的 MuJoCo/Node/浏览器版本、构建模式、准确源码与输入哈希、并发及显示/GPU 状态。不能把 offered-rate 上限当作最大容量，也不能将套接字、HTTP 与浏览器延迟混作同一数字。

## 证据与工作流程

归属方从最新远程集成提交建立聚焦 `refactor(v2)/<topic>` 工作树；EOS PR 目标为 `refactor(v2)/runtime-main`，同级仓库 PR 目标为 `master`。实现前，各归属方记录契约、fixture/负载设计与中英文首次使用步骤，并获得独立设计审查。独立代理审查实现、负例、安装证据和性能计算后才提交 PR。完整命令输出和 benchmark 原始数据保存在忽略的 `logs/test/<run-id>/`；跟踪的中英文证据页记录准确命令、环境、通过数量、初次失败和重跑处置。删除工作树前须将忽略日志及校验清单复制到评审人可访问的持久位置。Q03 验证已评审的主题分支提交及一次选定跨归属方安装重演。Q04 评审并压缩合并各 PR，验证远程目标，在合并提交上重演受影响边界，只删除已完成的主题分支和工作树，更新发布说明，之后才考虑 EOS 集成分支到 master 及选定标签。首次集成合并不能追认成 Q03 的主题分支证据。

## 选定主题提交结果

各归属方在其明确的提交上记录源码／安装关卡和固定负载基线；final-17 选定 Q01 矩阵针对五个准确主题提交通过六案例完整离线验证。[归属方证据索引](implementation-todo_cn.md#m5集成验收与发布)和[选定 EOS 证据](../../docs_cn/eos/maintenance/q03-selected-matrix-evidence_cn.md#final-17-选定主题提交结果2026-11-03)记录冻结选定文件、外部收据、命令、浏览器结果、Vision 帧及停止证明。准确 `219f031` EOS 源码关卡与选定源码验证通过，完整记录见[保留日志](../../docs_cn/eos/maintenance/q03-evidence_cn.md#选定跨仓库重演)。IsOS [PR #75](retired-repository-references_cn.md#ref-12d20c105aba) 通过选定归属方 HTTP 状态生命周期测试，覆盖 Apply、旧修订 Restart 拒绝、管理器替换和 Stop。EOS [PR #347](https://github.com/Island-Team/Island-eos/pull/347) 与 EGIA [PR #108](retired-repository-references_cn.md#ref-cfc2907c41c0) 增加独立封存的已安装 EGIA 托管进程及启动前反例证据，关闭该归属方安装边界。Q03 按这些固定并评审过的输入完成。Q04 独立的 [final-5 合并提交归档](q04-merged-qualification_cn.md#第五次尝试merged-home-final-5完整归档已验证)已通过；Q03 后 EOS 集成分支再次合入 master、公开快照、发布评审及选定标签仍待完成。托管 Origin 检查属于本机模拟，不证明公网 HTTPS 部署。
