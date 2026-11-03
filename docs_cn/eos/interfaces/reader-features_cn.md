# EOS 读取器特性与身份

[English](../../../docs/eos/interfaces/reader-features.md) · [节点配置](hardware_cn.md) · [Setup 适配器](setup-v1_cn.md) · [跨仓库 P05 任务](../../../docs/island/implementation-todo.md)

**P05 归属方契约——EOS 探针部分已实现。** IsOS 持有已保存的图、已验证的软件包 pin、部署预检与公共拒绝结果。EOS 声明所选可执行文件实际能读取什么。以下本地探针在任何 EOS 子进程启动前运行；返回读取能力，不代表实时健康或执行器授权。精确的内建 `supported_node_types`、setup 身份读取器字段与 `mujoco-child-v2` 已在此实现；按图推导准入与公共诊断仍属 IsOS 的 P05 工作。

## 探针与字段

IsOS 对精确选中的 `aether-profile`、`aether-runtime`、`aether-config-server` 文件执行 `--capabilities`，限制超时和标准输出大小，不经过 shell，也不提交图。每个探针输出一个 JSON 对象并以零状态退出，不打开 socket 或设备。`aether-profile setup --json` 还通过标准输入接收已有 schema-1 身份请求 `{"schema_version":1,"op":"identity","core":"eos"}`；其 `identity` 对象为同一个二进制文件返回相同的读取器集合。HTTP bridge 仅转发控制与 Vision 数据。选用该子进程时，IsOS 查询其传输特性；它的图／类型标量字段只是历史元数据，bridge 不是图读取器。

```json
{"product_version":"0.2.0","graph_version":"1.0.0","node_type_version":"1.0","supported_graph_versions":["1.0.0","0.1.0"],"supported_node_type_versions":["1.0","0.1"],"supported_node_types":["eos.control.manual@v1.0","eos.control.primitive@v1.0","eos.control.skill@v1.0","eos.dispatcher.control@v0.1","eos.dispatcher.control@v1.0","eos.dispatcher.primitive_base@v0.1","eos.dispatcher.primitive_base@v1.0","eos.dispatcher.primitive_manipulator@v0.1","eos.dispatcher.primitive_manipulator@v1.0","eos.hardware.base@v0.1","eos.hardware.base@v1.0","eos.hardware.camera@v0.1","eos.hardware.camera@v1.0","eos.hardware.manipulator@v0.1","eos.hardware.manipulator@v1.0","eos.interface.camera_gate@v0.1","eos.interface.camera_gate@v1.0","eos.process.backend@v1.0"],"binary":"aether-profile","features":["short-paths","node-types","data-idl","primitive-binding","catalog","setup-v1","graph-0.1-reader"]}
```

示例是有效 JSON；线上的 `supported_node_types` 数组按完整 ID 的字典序排列。冻结夹具列出每个读取器的精确有序数组。`aether-profile` 的 setup 身份回复将相同字段与 `core:"eos"`、`setup_schema_version:1` 放入现有的 `{"schema_version":1,"ok":true,"diagnostics":[],"files":[],"identity":{...}}`。无效身份请求仍返回现有 schema-1 `ok:false` 诊断，含 `path`、`code` 和 `message`，不返回文件。

| 字段 | 类型和单位 | 用途、缺省与校验 | 兼容性 |
| --- | --- | --- | --- |
| `binary` | 精确可执行文件名／— | 标识回答探针的所选文件；必须与 IsOS 调用的名称一致。 | setup 身份改用 `core:"eos"`。 |
| `product_version` | 语义版本字符串／— | 构建身份，用于展示和证据；不得为空。 | IsOS 不要求唯一的 EOS 产品版本。 |
| `graph_version`、`node_type_version` | 字符串／图版本与类型后缀版本 | 历史标量别名，目前为 `1.0.0`、`1.0`；不得扩展权威数组。 | 仅已知旧读取器在没有对应数组时使用下述标量回退。 |
| `supported_graph_versions` | 唯一字符串数组／精确图版本 | 本阶段精确接受 `1.0.0` 和 `0.1.0`。空数组、格式错误和重复项导致预检失败。 | 数组存在时具有权威性，不接纳未知未来版本。 |
| `supported_node_type_versions` | 唯一字符串数组／精确 `@v` 后缀版本 | 本阶段精确为 `1.0` 和 `0.1`；后缀不授权未列出的类型。 | 数组存在时具有权威性。 |
| `supported_node_types` | 唯一、有序字符串数组／精确 EOS ID | 下面的 18 个 EOS 内建注册项；不声明 IsOS、EGIA 或软件包贡献的 ID。 | P05 新字段。已知旧程序缺少该字段时，仅允许精确旧版回退，不接受 `@v0.1`。 |
| `features` | 唯一字符串数组／令牌 | 该二进制文件实现的协议与启动行为；产品版本不暗示特性。缺少必需令牌导致预检失败。 | 缺少数组不等于空特性集合；已知旧版回退范围很窄。 |
| `setup_schema_version` | 正整数／schema 版本 | 仅在 setup 身份中出现，目前为 `1`；标识既有请求／回复封装。 | 保留 schema 1 和已有诊断。 |

11 个旧版内建类型是 `eos.hardware.camera@v1.0`、`eos.hardware.base@v1.0`、`eos.hardware.manipulator@v1.0`、`eos.dispatcher.control@v1.0`、`eos.dispatcher.primitive_base@v1.0`、`eos.dispatcher.primitive_manipulator@v1.0`、`eos.control.manual@v1.0`、`eos.control.primitive@v1.0`、`eos.control.skill@v1.0`、`eos.process.backend@v1.0` 与 `eos.interface.camera_gate@v1.0`。七个新版内建类型是 `eos.hardware.camera@v0.1`、`eos.hardware.base@v0.1`、`eos.hardware.manipulator@v0.1`、`eos.dispatcher.control@v0.1`、`eos.dispatcher.primitive_base@v0.1`、`eos.dispatcher.primitive_manipulator@v0.1` 与 `eos.interface.camera_gate@v0.1`。`eos.process.mujoco@v0.1` 和历史 MuJoCo 专用类型由软件包贡献，不在静态列表中。IsOS 通过 `--node-types <path>` 将 P01 已验证的 sidecar 路径传给选中的 profile 程序，再请求 `setup catalog`；该精确结果校验贡献 ID、位置与设置。后缀或未验证 sidecar 均不足以授权。

## 按所选图确定必需特性

可执行文件探针保留现有特性令牌。`aether-profile` 声明 `short-paths`、`node-types`、`data-idl`、`primitive-binding`、`catalog`、`setup-v1`、`graph-0.1-reader`。`aether-runtime` 声明 `short-paths`、`node-types`、`data-idl`、`primitive-binding`、`log-dir`、`vision-gate`、`graph-0.1-reader`、`mujoco-child-v2` 和 `managed-control-grants`。`aether-config-server` 声明 `short-paths`、`node-types`、`data-idl`、`primitive-binding`、`catalog`、`graph-0.1-reader`。`aether-http-bridge` 声明 `short-paths`、`node-types`、`data-idl`、`primitive-binding`、`vision-ws`；它不解析图，因此不声明 `graph-0.1-reader`。为线协议兼容保留所示顺序，消费者按集合比较。

对 `0.1.0` EOS 投影，IsOS 要求 profile 工具及每个实际选中的图读取子进程（runtime、config server）都具备 `graph-0.1-reader`、`node-types` 和 `data-idl`。profile 工具还须具备 `setup-v1` 和 `catalog`。对所选的 `0.1.0` 受管理 EOS runtime，IsOS 还须在其进程 `/node_type` 精确要求 `managed-control-grants`；缺少时须在停止活动子进程或签发授权前返回 `reader_feature_unsupported`。该令牌只确认 EOS 可验证所选 schema-2 代际和按运行固定的授权；它本身不授予运动权限，也不暗示 EGIA、bridge 或模拟器能力。所选 runtime 启动带 `--log-dir` 时还要求 `log-dir`。图包含任一 EOS 原语绑定类型时要求 runtime 的 `primitive-binding`；包含 camera gate 时要求 runtime 的 `vision-gate`。

无论图版本如何，图通过 `eos.process.mujoco@v0.1`、`eos.process.mujoco@v1.0` 或将 `settings.package:"island/mujoco-sim"` 与 `settings.version:"0.1.1"` 配对锁定的旧版内建类型 `eos.process.backend@v1.0` 启动已验证 v2 MuJoCo 子进程时，要求 runtime 的 `mujoco-child-v2`。此令牌确认现有直系子进程 v2 socket 路径，不声称未来 R05 strict-socket 行为。每个启用的 `aether-http-bridge` 子进程均要求 `vision-ws`，即使图中没有 camera-gate 节点；停用的 bridge 不产生此要求。必需特性来自实际选中节点和启动参数；未出现的可选子进程或节点不产生特性要求。

对已知 `1.0.0` 图，使用精确旧版类型注册表与软件包 sidecar。没有数组的读取器仅在图中的 EOS ID 全部是已知旧版身份且按本契约计算的必需特性为空时，才可使用其 `1.0.0`／`1.0` 标量身份。仅有标量字段的 bridge 可以使用，因为它不读取图；无论图版本如何，每个启用的 bridge 仍须具备 `vision-ws`。格式错误的数组不得回退到标量。未知未来图版本、ID、类型后缀、无效探针与缺少必需特性均在启动前失败。此回退不改写已保存字节，也不授予图编排的技能序列执行能力。IsOS 现有固定的 `short-paths` 进程检查在这套派生特性集合之外继续生效。

IsOS 先检查已保存图与 P01 已验证 pin，然后探测精确选中的二进制文件与使用已验证 sidecar 的 EOS catalog，再进行归属方 Validate／Render 与生成快照验证；这些步骤都发生在停止活动子进程或启动替换进程之前。Run 和 Restart 根据不可变 P04 生成版本重新检查所选二进制文件的哈希与声明集合；可执行文件变更须显式 Apply 选取新生成版本。IsOS 持有公共 `{code,path,stage}` 诊断，指出可执行文件、节点 JSON Pointer、不支持的类型或特性和支持集合。EOS setup 保留 schema-1 `{path,code,message}` 诊断，不输出原始图、本地路径、密钥值或软件包内容。只有 IsOS 确知精确匹配且已验证的兼容发行版时才推荐升级版本，否则仅报告所需支持，不猜测产品版本。

[冻结的 P05 夹具](../../../tests/fixtures/reader_features/README.md)覆盖身份、特性推导、已知旧版回退、精确贡献类型和预检负例。实现必须比较真实选中的二进制文件，而非模拟版本字符串；P05 关口关闭前需重放一个 `0.1.0` 和一个安全的 `1.0.0` 图。

**Q01 目标设计——尚未实现：** IsOS 选定 `vision_evidence.gate_path` 时，还要求选定 `aether-runtime` 精确声明 `vision-provenance-v1`，选定 `aether-http-bridge` 精确声明 `vision-owner-uds-v1`。前者证明原子的 `vision_attest`、已保存／投影版本、stream 与帧元数据；后者证明独立的所有者 UDS listener、运行范围 bearer 校验及公共／所有者路由隔离。既有 `vision-gate` 和 `vision-ws` 不蕴含这两个特性。缺少时在相应选定进程的 `/node_type` 返回 `reader_feature_unsupported`，早于授权签发或活动子进程替换。当前二进制没有声明这些 Q01 令牌。
