# EGIA 读取器与受管运行时特性

[English](../../../docs/ei/interfaces/reader-features.md) · [IsOS setup](isos-config.md) · [阶段契约](pipeline.md) · [跨仓库 P05](../../island/implementation-todo_cn.md)

**P05 EGIA 归属契约。** 本检出已实现 setup 身份和不启动运行时的探针；IsOS 准入仍是独立的集成关卡。IsOS 持有已保存图、精确修订、所选可执行文件、软件包固定版本、生成配置和准入。EGIA 报告所选 setup 适配器能读取的内容以及所选 Python 运行时能启动的行为。能力回复不证明 EOS gRPC Gateway 已就绪，也不证明 IsOS 已准入新的 EGIA 阶段图。

## 精确选择的探针

IsOS 向选中的 `egia-config setup --json` 标准输入发送 `{"schema_version":1,"op":"identity","core":"egia"}`。现有 schema-1 成功信封保持空的 `diagnostics` 和 `files`。`identity` 增加 `supported_node_types` 和 `features`，保留 `core`、`product_version`、旧版标量别名 `graph_version:"1.0.0"`、`node_type_version:"1.0"`、`supported_graph_versions:["0.1.0","1.0.0"]`、`supported_node_type_versions:["0.1","1.0"]` 及 `setup_schema_version:1`。下面保留历史 P05 集合；A01 还声明[阶段目录](pipeline.md)中的七种精确 `@v0.1` 身份。完整 identity 数组按字典序排序：

1. `egia.common.context@v1.0`，位置 `egia/common/data`；
2. `egia.common.guard@v1.0`，位置 `egia/common/process`；
3. `egia.common.planner@v1.0`，位置 `egia/common/process`；
4. `egia.common.source@v1.0`，位置 `egia/common/data`。
5. `egia.context.builder@v0.1`，位置 `egia/cognition/context`；
6. `egia.decision.planner@v0.1`，位置 `egia/cognition/decision`；
7. `egia.link.gateway@v0.1`，位置 `egia/io/link`；
8. `egia.missions.queue@v0.1`，位置 `egia/cognition/missions`；
9. `egia.monitor.health@v0.1`，位置 `egia/governance/monitor`；
10. `egia.perception.admission@v0.1`，位置 `egia/evidence/perception`；
11. `egia.safety.gate@v0.1`，位置 `egia/governance/safety`。

Setup 特性数组是 `["setup-v1","catalog","graph-0.1-reader","egia-evidence-routing-v1"]`。`setup-v1` 表示存在 schema-1 identity、catalog、Validate 与 Render 交换；`catalog` 表示现有配置档案／默认值目录可用；`graph-0.1-reader` 表示适配器可检查 `0.1.0` 信封及它声明的精确 ID。`egia-evidence-routing-v1` 表示 Validate/Render 可编译 `catalog.routed_edges` 中两条精确的证据路径；每条目录记录有 `source_type`、`output`、`target_type`、`input` 和 `controls: ["priority","frequency_hz"]`。A01 可渲染完整七阶段图；四个历史设计节点仍不可执行。IsOS 须完成独立的 reader/catalog 更新才可准入新 ID 与路径。EGIA setup 拒绝时仍返回现有 `{path,code,message}` 诊断和空 `files`。路径包含请求包装层，例如 `/graph/cores/egia/layers/common/groups/data/nodes/source/node_type`；IsOS 在公共诊断中映射为不带 `/graph` 的保存图指针。

IsOS 使用**同一个所选** `egia` 启动器执行 `--capabilities`。它输出一个有界 JSON 对象并以零状态退出，不加载生成配置、不导入流水线、不打开套接字、不启动 EOS 发现、不初始化虚拟环境，也不写文件。目标回复为 `{"binary":"egia","product_version":"0.1.0","features":["managed-config-v1","eos-gateway-client-v2","egia-evidence-scheduler-v1","vision-evidence-ingress-v1","vision-inspection-uds-v1"]}`。`managed-config-v1` 证明 `--managed --no-tui --mode isos` 接受 P04 生成的 `EGIA_ETC` 树，并拒绝内部启用的 sandbox。`eos-gateway-client-v2` 证明现有针对 `island.eos.v1.GatewayService` 与 `island.eos.v2.CapabilityService` 的类型化客户端；它不证明 EOS 实现了这些服务、发现已完整或 EGIA 已就绪。 `egia-evidence-scheduler-v1` 证明所选托管进程包含这两条精确路径的进程内最新值调度器；它不代表 IsOS 图已准入。 运行时不解析部署图，因此**不**声明图版本、类型版本或节点类型数组。IsOS 在 `egia-config` 检查这些数组，再独立检查运行时所选启动特性。

| 字段 | JSON 类型／单位 | 用途及校验 |
| --- | --- | --- |
| `core` | 精确字符串 `egia`／— | Setup 身份归属；与可执行文件名独立。 |
| `binary` | 精确字符串 `egia`／— | 运行时探针身份；须匹配所选进程角色。 |
| `product_version` | 非空字符串／发行版本 | 仅用于显示与溯源；不能只凭某一版本判定兼容。 |
| `graph_version`、`node_type_version` | 字符串／版本 | 历史标量别名；不得扩大权威数组。 |
| `supported_graph_versions` | 非空、唯一的字符串数组／精确图版本 | Setup 只接受 `0.1.0` 和 `1.0.0` 信封。 |
| `supported_node_type_versions` | 非空、唯一的字符串数组／精确后缀版本 | Setup 接受 `0.1` 及历史 `1.0`；仅匹配后缀不能准入类型。 |
| `supported_node_types` | 唯一且排序的字符串数组／完整 ID | Setup 的四个历史 common ID 及七个精确 EGIA 阶段 ID；不声明软件包类型。 |
| `features` | 唯一的 ASCII 字符串数组／特性令牌 | 已实现的探针特定行为；缺失表示未声明特性。 |
| `setup_schema_version` | 正整数／schema | Setup 请求与回复 schema 仍为 `1`。 |

IsOS 在有界超时和输出限制下探测将要启动的精确启动器及解析后的解释器。Shell 包装器在能力探测时不得初始化 Python 环境。所选启动器、解释器、相关环境绑定与已安装 EGIA 代码共同构成一个经过验证的部署输入；探测某个 Python 安装不能授权另一个。探测、P04 生成版本校验与启动之间任一输入变化，都须在停止活动子进程前拒绝。解释器无法读取，或探针非零退出、格式错误、字段重复、输出过大、程序身份错误，均关闭准入。探针 stderr 与本地路径不得复制到公共诊断。

所选启动链依赖 `EGIA_PYTHON`、`ISLAND_PYTHON`、`ISLAND_PREFIX`、`ISLAND_LIB`、`ISLAND_LIBEXEC`、继承的 `PYTHONPATH`，以及源码／安装布局。包装器把所选根目录的模块目录放在 `PYTHONPATH` 前面；解释器从最终路径导入 `egia.reader_features`。IsOS 为 P04 生成版本固定规范启动器、解析后的解释器、有效的非秘密环境绑定和所选模块树，然后在替换活动进程前重新校验。源码检出缺少 `.venv/bin/python` 时，`--capabilities` 以 `127` 退出，不运行 `uv sync` 或创建虚拟环境。

## 推导与失败边界

对启用 EGIA 的 `0.1.0` 图，IsOS 要求 setup 的 `setup-v1`、`catalog`、`graph-0.1-reader`、精确图／类型版本以及每个完整 EGIA 节点 ID。对任一图版本，受管 EGIA 启动要求运行时 `managed-config-v1` 与 `eos-gateway-client-v2`。仅有四个历史 ID 的安全 `1.0.0` 图，只有在三个版本／类型数组**全部缺失**且标量别名恰为 `1.0.0`／`1.0` 时，才允许已知旧版 setup 身份回退。部分存在或格式错误的数组必须失败。此回退不能准入未知 ID 或特性，Run／Restart 仍须探测所选运行时。单独有效的 setup 身份不能使 EGIA 进程可启动。

IsOS 在 Render、生成版本选择、停止活动子进程或启动之前，检查实际保存图字节、注册位置、精确软件包固定版本与所选读取器集合。已注册 ID 若不在所选读取器集合中，产生 IsOS `reader_type_unsupported`；未知／未来图或类型保留先前 G01/G05 精确路径错误码。缺失特性产生 `reader_feature_unsupported`；探针身份错误或格式错误产生 `reader_binary_mismatch` 或 `reader_identity_invalid`。IsOS 返回其 P05 `{code,path,message,stage,reader}` 诊断与支持集合；EGIA setup 保留 schema-1 诊断。只有 IsOS 验证了某个精确兼容发行版及其资产摘要，才可指名升级。`0.1.0` 支持、进程 PID 与已绑定监听器均不代表 R02 Gateway 就绪或完整的受管阶段启动。

[冻结夹具](../../../ei/tests/fixtures/reader_features/README.md)覆盖所选探针身份、四个 ID 与位置、旧版回退、不支持的阶段 ID、缺失运行时特性、格式错误的探针、变更的解释器与拒绝时不停止。A01 增加独立的 EGIA 阶段测试；IsOS 所选 reader 准入仍是独立关卡。

**Q01 已实现 EGIA 探针：** 选定 Vision 证据绑定还要求精确受管 EGIA runtime 的 `--capabilities` 响应包含 `vision-evidence-ingress-v1` 和 `vision-inspection-uds-v1`。前者证明有界且认证的 EOS 所有者 UDS 订阅者，后者证明独立的只读 EGIA 检查 UDS 与授权。既有 A04 证据路由不蕴含任一特性。缺失令牌时 IsOS 在签发运行授权或替换子进程前拒绝。参见 [Vision 证据](vision-evidence.md)。
