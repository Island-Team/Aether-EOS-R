# 已验证软件包准入

[English](../../../../docs/isos/interfaces/packages/verification.md) · [发行包](release_cn.md) · [资源](resources_cn.md)

安装验证一个精确 `{id,version}` 发行包与工件，然后原子发布收据和载荷。本地链接冻结被接受元数据与载荷指纹。IsOS 在图准入、Apply、Run 与 Restart 前重新验证；变更后的链接源需要明确重新链接。图未锁定的无关已安装软件包不能阻止该图。

准入采用单一顺序：精确锁定 → 发行契约及载荷收据 → 注册节点类型与位置 → 所声明资源 ID 和受控文件摘要 → 所有者与生命周期能力 → 绑定运行的交接。锁定收集器覆盖所有受支持的图位置，重复诊断按规范图指针排序。不存在软件包特定优先级或后备解析器。

| 失败 | 结果 | 状态影响 |
| --- | --- | --- |
| 缺少必需的通用贡献 | `unsupported_package_contract` | 不更改保存的图或活动运行。 |
| 发行包无效、不安全路径、符号链接、摘要不符或元数据过大 | 带图指针和阶段的有界软件包诊断 | 不发生部分安装或替换。 |
| 未知注册类型、位置、所有者或适配器 | 类型化准入诊断 | 不启动子进程，不发布图。 |
| 未知资源 ID 或资源字节变化 | 类型化资源诊断 | 不启动子进程，不发布图。 |
| 保存修订或运行栅栏过期 | 冲突诊断 | 不替换活动运行。 |

直接安装/链接错误使用空图指针；图上错误使用实际选中节点的指针。诊断包含稳定代码、阶段、精确锁定、净化消息及规范指针，不泄露本地路径、原始元数据或秘密。响应封套见[HTTP 软件包](../../develop/http/packages_cn.md)。

## 读取器与功能预检

IsOS 在 Apply、Run 与 Restart 前检查选中的可执行文件及其精确已验证软件包贡献。产品版本、其他检出、可执行路径本身或软件包类型后缀都不能证明支持。能力探测有界、不启动运行时，并必须描述实际启动字节。可执行文件、解释器、模块树、软件包收据或生成修订变化时阻止替换。解释器缺失不会触发 `uv sync` 或其他引导。外部设置响应使用模式 2；IsOS 自有 HTTP 与软件包契约独立使用 `"0.1.0"`。

## 读取器身份字段

| 字段 | JSON 类型 / 单位 | 用途、缺省及验证 |
| --- | --- | --- |
| `supported_graph_versions` | 非空唯一字符串数组 / 图版本 | 选中二进制实际读取的精确版本。选中读取器声明支持图版本时必需。 |
| `supported_node_type_versions` | 非空唯一字符串数组 / 类型后缀 | 二进制读取的精确后缀；不足以准入完整类型 ID。 |
| `supported_node_types` | 唯一完整 ID 数组 / 类型 | 读取器实现的内建 ID。贡献类型还需要已验证精确软件包锁定和所有者目录。 |
| `features` | 唯一 ASCII 字符串数组 / 能力 | 该二进制实际能力。缺失表示空集；所需能力随后失败。 |
| `binary` | 字符串 / 选中可执行身份 | 运行时 `--capabilities` 角色，例如 `aether-runtime`；设置使用 `core`。 |
| `product_version` | 非空字符串 / 发行版本 | 仅用于展示与来源，不是兼容性判据。 |
| `setup_schema_version` | 整数 / 外部设置模式 | 当前选中设置身份必须为 `2`。 |

IsOS 在消费合并所有者目录前检查图结构、精确锁定和经过验证的贡献。随后依据选中的设置读取器、图读取运行时及传输子进程检查图/类型版本与所需功能。`package-process-v1` 贡献需要所有者能力；选中运行时还可能根据已准入图要求 `managed-control-grants`、`managed-config-v1` 或 `eos-gateway-client-v2`。缺少功能时在启动前阻止。所有者声明功能不证明就绪；健康状态另行检查。诊断按规范图路径、二进制角色及功能 ID 排序。

## 诊断字段

| 字段 | JSON 类型 / 单位 | 用途与验证 |
| --- | --- | --- |
| `code` | 稳定字符串 / 失败 | 为 `reader_identity_invalid`、`reader_graph_unsupported`、`reader_type_unsupported`、`reader_feature_unsupported`、`reader_binary_mismatch` 或 `reader_package_pin_mismatch`。 |
| `path` | JSON 指针 / 图字段 | 精确选中类型/软件包或 `/version`；选中子进程错误可用 `/settings/role`。 |
| `message` | 有界字符串 / 修复文本 | 净化后的操作，不含原始 stderr、本地路径或秘密。 |
| `stage` | `graph`、`package`、`reader`、`feature` / 阶段 | 标识失败的准入步骤。 |
| `reader.core`、`.binary`、`.binary_sha256` | 字符串及可选 64 位十六进制 / 选中二进制 | 精确选中所有者、可执行身份及字节来源。 |
| `reader.requested` | 对象 / 请求声明 | 适用的图版本、完整类型、功能及精确软件包锁定；不适用值为 null。 |
| `reader.supported` | 对象 / 已声明能力 | 排序后的图版本、类型版本、完整 ID 及功能；不可信声明为空数组。 |

有界诊断示例（JSON 对象与英文文档相同）：`{"diagnostics":[{"code":"reader_feature_unsupported","path":"/cores/eos/layers/interface/groups/gate/nodes/preview/node_type","message":"selected owner lacks vision-gate; choose a compatible build","stage":"feature","reader":{"core":"eos","binary":"aether-runtime","binary_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","requested":{"graph_version":"0.1.0","node_type":"eos.interface.camera_gate@v0.1","feature":"vision-gate","package":null},"supported":{"graph_versions":["0.1.0"],"node_type_versions":["0.1"],"node_types":["eos.interface.camera_gate@v0.1"],"features":[]}}}]}`。经过验证的 `compatible_upgrade` 只能命名精确检查过的发行包；列出它不会安装或应用。所有者迁移门槛见[待办](../../maintenance/todo_cn.md)。
