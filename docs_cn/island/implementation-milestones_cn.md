# 0.x 实现里程碑

[English](../../docs/island/implementation-milestones.md) · [架构](architecture_cn.md) · [层连接](layer-dataflow_cn.md) · [详细 TODO](implementation-todo_cn.md) · [M2 验收计划](m2-qualification_cn.md) · [M3 验收](m3-qualification_cn.md) · [EOS 待办](../eos/maintenance/todo_cn.md)

**状态：M1–M5 在所述 0.x 范围内已完成；本 PR 完成最终远程私有 `master` 同步。** [M2 证据](m2-qualification_cn.md)记录精确锁定的合并输入下六批证明通过 28/28 条用例。[限定范围的 M3 证据](m3-qualification_cn.md)记录 158 文件的精确合并版本重放、当前提交聚焦检查和封存的 R01–R07 所有者证据。[M4 完成证据](m4-qualification_cn.md)把经审查的 EGIA 与 IsosUI 归属方测试对应到固定版本的安装后任务、浏览器、故障、恢复及 Stop 重放。这些是 EOS `0.2.0`、EGIA/IsOS/IsosUI `0.1.0` 与 `island/mujoco-sim@0.1.1` 的依赖关卡。只有契约夹具、故障和停止测试以及相应首次使用步骤均通过，才关闭里程碑。产品 WorkflowService、ActionList DAG 调度和 workflow 模板**不属于本轮 0.x 目标**。历史 protobuf 名称和标签仅作为兼容身份保留；不新增 workflow 节点、路由或 UI。

| 关卡 | 按顺序实现 | 完成证据 |
| --- | --- | --- |
| **[M1——版本化图与节点目录](https://github.com/Island-Team/Island-eos/milestone/1)** | IsOS/EOS 安全读取含既有 `@v1.0` 和受支持 `@v1.1` 类型的 `1.0.0` 图，拒绝未知类型版本；新建 `0.1.0`/`@v0.1`；EGIA/IsosUI 读取两者；EOS 与已注册包侧文件贡献描述符，历史 EGIA 设计身份保持冻结读取；定义类型描述、字段/端口类型与单位、指向连线的验证及显式迁移。 | 新旧图夹具、迁移前保存字节不变、不支持版本诊断、任意命令不启动、目录、侧文件及拒绝连线测试。 |
| **[M2——已验证部署输入](https://github.com/Island-Team/Island-eos/milestone/2)** | IsOS 精确注册表收据、包专属节点集/资源、选择器、生成 EOS/EGIA 配置、XDG 根、版本预检和统一 JSONL 聚合；在管理器会话关联下记录精确包来源与启动前验证失败。MuJoCo 包验证场景/设备 ID 与资源。 | 全新主目录安装→选项→校验/应用；篡改资源与不可用选项失败；来源、日志轮换/脱敏/游标及版本不匹配测试。 |
| **[M3——受治理执行](https://github.com/Island-Team/Island-eos/milestone/3)** | EOS Rust gRPC 健康/发现/能力 gateway、带健康/新鲜度检查的唯一 dispatcher 路径、有界连线优先级/频率，以及外部准入默认拒绝的版本化 EOS **宿主技能 ABI**；MuJoCo 适配器和子进程验证请求、停止并回收。 | EGIA stub 只有完整发现后才就绪；未配置/无效/未声明技能请求被拒绝；假时钟准入、重启不重试、QoS 公平性/溢出与仿真器故障/不确定命令/安全停止测试。 |
| **[M4——智能体与工作台](https://github.com/Island-Team/Island-eos/milestone/4)** | EGIA Python link→perception→context→missions/decision→safety→monitor 类型化阶段；IsosUI 注册表驱动节点编辑器、悬停/焦点浮动节点信息卡、每项活动绑定一项控制的原语演示、连线类型/单位与区别于实时健康的验证状态，以及独立浮动 control/monitor/Vision/log 面板。 | 阶段单元和 stub 集成测试；UI 选择器、不可用值、拒绝连线文本、键盘/无障碍、原语请求/结果轨迹、过期修订和布局持久化测试。 |
| **[M5——集成验收与发布](https://github.com/Island-Team/Island-eos/milestone/5)** | 全新主目录 IsOS→EOS→MuJoCo→EGIA 运行、重启恢复、安全停止、使用指南复现，以及最后调用者检查后的无用 C++ 运行时清理。保留活跃 CXX mock、适配器 C ABI 及所需 schema。 | 完整输出位于 `logs/test/<run-id>/`；格式/lint/测试、注明环境的固定负载吞吐/中位数/p99、已审查专题 PR 和发布说明。选定的 0.x 标签及准确标签的发布后验收完成了所述 M5 关卡。 |

产品实施队列：[EOS 0.2.0](https://github.com/Island-Team/Island-eos/milestone/6)、[IsOS 0.1.0](retired-repository-references_cn.md#ref-007ed27ec68b)、[EGIA 0.1.0](retired-repository-references_cn.md#ref-0212e6fd1019)、[IsosUI 0.1.0](retired-repository-references_cn.md#ref-edd683471a30)、[MuJoCo 包 0.1.1](https://github.com/Island-Team/isos-mujoco-sim/milestone/1)。这些跟踪实施工作，不改变图或线协议版本。

每个关卡可使用多个聚焦的 `refactor(v2)/<topic>` 分支。跨仓库所有者用同一完成证据结束各自部分；后续关卡通过不能代替前置夹具。IsOS 拥有已应用修订和包锁定，EOS 拥有已准入执行，EGIA 拥有智能体状态，IsosUI 拥有呈现，MuJoCo 拥有模拟器套接字。具体字段以所属 `interfaces/` 页面为准，而不是本进度表。

## 里程碑交付循环（开发 agent）

每次按此循环推进**一个 M1–M5 里程碑**。这是贡献者与审查 agent 的工作安排，不定义部署节点、WorkflowService 或 ActionList。里程碑内的每个重要实施切片都重复设计与审查步骤。

1. **让 agent 规划。** 给规划 agent 分配不同的所有者或边界问题，让其检查当前代码、所属契约、依赖及上表中的里程碑要求。汇总为按依赖排序的实施切片计划，写明范围、非目标、负责方、兼容风险及该行的完成证据。实施前在详细 TODO 或关联的所有者设计中记录切片计划和已解决的决策。
2. **设计契约与首次使用。** 各所有者更新权威 `interfaces/` 契约、相关设计与算法页面、夹具及中英文使用步骤。区分当前行为与目标行为。操作者指南遵循设置 → Apply → 就绪 → 控制 → 因果日志 → 停止的顺序。
3. **独立检查设计。** 由未编写该设计的 agent 将每份变更设计与所属契约、schema、现有使用方及相邻层数据流比较。检查版本兼容、字段校验、故障、重启与安全停止。在设计或 PR 中记录审查发现及处理结果；解决问题并复查后，才认为设计就绪。
4. **分片实施。** 将每个切片分配给实施 agent，并按仓库 agent 指南使用聚焦的 `refactor(v2)/<topic>` 分支。依照已审查契约实施；改变行为的代码、契约更新及聚焦的单元/mock/失败测试放在同一个实施 PR。可先提交仅含设计文档的规划 PR，但它不代表 TODO 已完成。
5. **运行集成证据。** 聚焦检查后，使用必要的 stub 或模拟器运行该里程碑受影响的已安装边界；M5 运行完整的 IsOS → EOS → MuJoCo → EGIA → IsosUI 路径。按适用情况覆盖故障、重启与安全停止。把完整命令和输出保存在 `logs/test/<run-id>/`，对照里程碑完成条件及首次使用的日志顺序。
6. **审查并合并。** 由独立 agent 审查代码、契约、测试与集成证据；修复问题并重跑受影响检查。审查 PR，squash merge 到集成分支，确认远端目标已更新；保留日志后仅移除已完成的专题分支或工作树。
7. **关闭里程碑。** 重放该里程碑适用的中英文首次使用步骤；M5 从全新 IsOS 主目录重放完整指南。只勾选已验证的 TODO，更新当前/计划状态及证据摘要；该行全部完成条件通过后才关闭里程碑。未满足的条件回到相关设计或实施步骤。

## M3 完成决定

[M3 验收记录](m3-qualification_cn.md)锁定 EOS `2437f81a`、IsOS `b5553ad9`、MuJoCo `4e6b5449` 及 IsosUI 源码 `40297289`。真实 pick-place 和 navigation 控制、选中 Stop 收据、停止套接字丢失后的未知路径、管理器重启栅栏、当前聚焦测试与经审查的所有者负例满足 M3 完成条件。独立重放评审支持此限定范围的关闭。实时物理部分证明、Stop 进行中管理器死亡及跨所有者的就绪后 GUI 渲染阻塞仍是明确边界。

## M4 完成决定

[经审查的 M4 完成记录](m4-qualification_cn.md)把 EGIA 四项阶段切片及 IsosUI 四项工作台切片对应到归属方测试、历史选定 M4 任务／浏览器和恢复反例归档，以及后续选定故障／Restart／Stop 重放。按固定输入的明确边界关闭原定 M4 条目；不宣称同一次运行内 EGIA 子进程替换或公网 HTTPS Origin 检查。在 M4 完成时，M5 公开构件及发布验收尚待完成；下述后续 Q04 证据使其关闭。

## 操作者首次使用顺序

验收运行按使用指南进行：安装构建和精确包→选择节点/资源→校验并应用→尚未启动时先看管理器会话最初的包/Apply 故障→启动及确认实时状态→查看节点并调用一个活动原语绑定→读取部署运行最初的就绪或故障事件，再按关联 ID 过滤→停止→通过显式迁移升级。状态是实时权威信息；启动日志是状态变化的证据。每个模块只输出一条有用就绪事件，之后记录状态变化，使首次使用日志易读。

## 设计审查与验证

初始架构、宿主 ABI、节点目录和其他产品页面经过独立 agent 审查；已处理历史技能身份、节点位置、加载器发现、套接字所有权及严格模式兼容问题。那一轮仅改文档：五个仓库均通过 `git diff --check`、变更文件 JSON/TOML 解析和全仓相对链接检查；完整输出位于 `logs/test/20261101-docs-node-architecture/docs-check.log`。后续根目录待办合并及部分实现见[详细 TODO 验证记录](implementation-todo_cn.md#验证记录)；这些记录不代表剩余里程碑任务完成。

五个选定公开产品标签已于 2026-11-04 发布，准确标签对象及提交见 [Q04 发布后记录](q04-merged-qualification_cn.md#发布后五标签验收2026-11-04已完成)。独立评审结合已验收的 final-5 合并提交六案例矩阵与 run-7 封存的公开构件首次使用、Restart、迁移及安全 Stop 证据批准关闭。[Q04 记录](q04-merged-qualification_cn.md#第七次尝试公开归属方构件2026-11-04已批准)注明所选配置与 Vision 边界。

首次公开标签运行保留了通过的 Restart 与迁移验证结果，但独立评审发现实际提供的 UI 字节与已发布 UI 构件不同，IsOS 与 EGIA 的实时进程也来自源码构建。另行选定的 run-7 已发布构件重放覆盖了 run-6 的这一边界。

[Run-7 已发布构件重放](q04-merged-qualification_cn.md#第七次尝试公开归属方构件2026-11-04已批准)使用发布版 IsOS 二进制、UI 归档及已安装 EGIA wheel，通过本地主路径、Restart、迁移和 Stop。本地构件验证器在重新封存前通过；独立评审验证了最终 314 成员的封存。UI 未选 Vision 来源，面板保持 `Stopped`；所选 EGIA 配置为所选公开检出目录改写一个 EOS share 条目。修正后的收据记录该来源说明。Final-5 与 run-7 的组合证据满足 M5 关卡，不声称 UI Vision 帧已渲染或公网 HTTPS。本 PR 将经验收的 `refactor(v2)/runtime-main` Git tree 最终同步到远程私有 `master`。
