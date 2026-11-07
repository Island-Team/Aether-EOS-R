# 供 IsOS 使用的 EGIA 配置接口

[English](../../../docs/ei/interfaces/isos-config.md)

`bin/egia-config` 是只读且无副作用的命令行接口。它复用 EGIA 的配置模型，但不会启动决策流水线、连接 EOS、监听端口或写入文件。IsOS 负责配置服务器，并原子写入返回的文件。

`setup` 是 v1 按需配置接口。IsOS 从标准输入传入一个 JSON 对象：`{ "schema_version": 1, "op": "catalog|validate|render", "core": "egia", "revision": "<已保存的图版本>", "graph": <完整 0.1.0 或旧版 1.0.0 图>, "endpoints": {"egia_northbound": {"host": "127.0.0.1", "port": 16060}, "eos_gateway": {"transport": "tcp", "host": "127.0.0.1", "port": 16022}}, "packages": [] }`。请求上限为 1 MiB、已解析包最多 256 个。返回 `{ "schema_version": 1, "ok": true, "diagnostics": [], "files": [...] }`；`catalog` 还在 `catalog` 中返回现有节点档案和默认值。`validate` 不返回文件，`render` 返回三个旧版覆盖文件，完整新阶段图另增加绑定修订的 `egia/stages.json`。只有 IsOS 会将文件写入其托管配置目录。已保存图必须包含 EGIA core；图版本冲突检查和包验证由 IsOS 负责。

在加载图之前检查已配置的程序时，IsOS 向 `egia-config setup --json` 发送 `{ "schema_version": 1, "op": "identity", "core": "egia" }`。此操作不接受图、修订号、端点或包。成功响应除常规的 `schema_version`、`ok`、空 `diagnostics` 和空 `files` 外，还包含 `identity: { "core": "egia", "product_version": "0.1.0", "graph_version": "1.0.0", "node_type_version": "1.0", "supported_graph_versions": ["0.1.0", "1.0.0"], "supported_node_type_versions": ["0.1", "1.0"], "setup_schema_version": 1 }`。`product_version` 是 EGIA 包版本；其余版本分别标识支持的 IsOS 图、节点类型和 setup 接口。IsOS 用这些值报告兼容性和启动就绪状态。无效的 identity 输入以退出码 2 结束，返回诊断且不生成文件。

`eos_gateway` 必须指向已验证的 EOS **gRPC** 端点，可为包含 `host`、`port` 的 TCP，或包含绝对 `path` 的 UDS。`16020` 的 EOS HTTP 控制接口和 `16021` 的目录接口不能替代 gRPC。没有网关时 `catalog` 仍可用，但 `validate`、`render` 返回诊断并以退出码 2 结束；EGIA 运行需等待兼容网关配置完成。

## 当前单核心 setup 读取器（0.2.1 源码）

IsOS 向 `ei/bin/ei-config setup --json` 发送 setup schema `2`，兼容所有者角色仍为 `core:"egia"`。对 `0.2.0` 图，`graph` 是唯一 `isos` 核心中仅含 `ei_*` 层的只读投影；`revision` 是已保存源图的小写 SHA-256。精确阶段 ID 为 `isos.ei_<layer>.<name>@v0.2`。Identity 报告产品 `0.2.1`、图 `0.2.0`、节点类型 `0.2` 和可读取的旧版本。Catalog 对 `0.2.0` 投影返回新阶段 ID。Validate 与 render 检查位置、设置、进程内边和 EOS gateway 一致性，不修改源图。渲染的 `ei/stages.json` 保留精确的 0.2.0 EI 所有者投影和请求修订号；EI 进程在内存中将其适配为阶段运行时输入。其他托管覆盖文件也写在 `ei/` 下。旧 `egia-config` 命令与 `0.1.0`/`1.0.0` 图读取器继续可用。下方旧 setup 示例和注明日期的证据记录历史接口。

## 操作

运行 `egia-config describe|defaults|profiles|render|validate --json`。每次操作输出一个 JSON 对象，包含 `schema_version: 1`、`ok`、`data` 和 `diagnostics`。诊断包含 `path`、`code`、`message`；输入无效时退出码为 2。`describe` 返回 EGIA 产品版本和所需 EOS 协议服务。`profiles` 返回版本为 `1.0.0` 的 `egia.agent` 节点档案，其 EOS gRPC 输入声明了所需协议。节点档案版本与产品版本相互独立。

`render` 和 `validate` 从标准输入读取一个 JSON 对象：

节点档案还向图编辑器提供有序的 `settings_fields`。每个描述项使用相对于 `values` 的点分路径 `name`，并包含 `type`、`label`、`description`，以及适用的 `default`、`required`、`min`、`max`、`exclusive_min` 或 `options` 约束。顺序依次为监听地址、监听端口、状态频率、EOS 传输方式、EOS 地址、EOS gRPC 端口和 EOS 套接字路径。例如监听端口描述项为 `{"name":"server.port","type":"integer","label":"Listener port","description":"Port for the EGIA northbound WebSocket listener.","default":16060,"min":1,"max":65535,"required":true}`。`gateway.host` 和 `gateway.grpc_port` 适用于 TCP；`gateway.path` 适用于 UDS。IsOS 从连接的 EOS 节点推导 UDS 路径，因此应显示该值且避免冲突编辑。`defaults` 操作返回包含 `server` 和 `gateway` 对象的嵌套结构，与下方 `values` 的形状一致。旧版 `fields` 映射保留供现有调用方使用。

```json
{
  "schema_version": 1,
  "values": {
    "mode": "isos",
    "server": {"host": "127.0.0.1", "port": 16060},
    "gateway": {"transport": "uds", "path": "/run/user/1000/isos/deployments/main/eos.sock"}
  }
}
```

`validate` 校验可编辑值并返回规范化结果。`render` 的 `data.files` 包含相对于 IsOS 托管配置根目录的 `path` 和 `content`：`egia/egia.toml`、`egia/modes.toml`、`egia/profiles/isos.toml`。在应用这些覆盖文件之前，IsOS 必须从安装或源码中的 `etc/egia` 准备其余 EGIA 配置，包括 `prompts.toml`。IsOS 以该目录作为 `EGIA_ETC`，并使用 `--mode isos --managed --no-tui` 启动 EGIA。生成的路径均位于托管根目录内。

TCP 网关格式为 `{"transport":"tcp","host":"127.0.0.1","grpc_port":16200}`。未设置 `transport` 的现有配置仍为 TCP。UDS 格式要求绝对 `path`，拒绝 TCP `host`，并直接将 `unix:///absolute/path` 交给 gRPC。IsOS 创建私有套接字目录并先启动 EOS；EGIA 不经由 IsOS 转发运行时调用。运行时监听默认使用回环端口 `16060`。

## 默认端口验证

**Q01 所有者绑定：** IsOS 另行选择一个私有 EOS Vision 端点，并将独立的[只读 Vision 绑定](vision-evidence.md)写为 `egia/vision-binding.json` 与所有者授权文件。该绑定位于图及 schema-1 `egia-config setup` 响应之外。EGIA 受管运行时现已独立于现有 gRPC `eos_gateway` 校验部署运行、图版本、gate、来源、渲染及授权。Vision 端点不能替代 gRPC gateway，也不能假定固定 `16020`/`16021` 端口。选定 IsOS 集成和 Q01 实际回放仍属独立关卡。

EGIA 监听地址为 `127.0.0.1:16060`；IsOS 节点档案、独立配置、TUI 和可选 sandbox media route 使用相同默认值。EOS 南向连接独立配置（旧独立示例使用 `16003`，IsOS 托管时使用已验证的 gRPC 端点）。可选 sandbox EOS gRPC 网关默认使用 `16022`；显式配置的端口保持不变。EGIA 监听端口与 IsOS 的 `16000`/`16001`、EOS 的 `16020`/`16021` 不冲突。

修改此接口后运行 `uv run ruff check .`、`uv run pytest`、`uv run pyright`、`PYTHONPATH=src:libexec uv run lint-imports` 和 `./bin/islandctl validate`。2026-10-31 的首次公开接口版本修改通过全部五项检查（806 个测试），完整输出位于 `logs/test/20261031-egia-public-v1/`。EOS 自有的 gRPC 服务名称及版本独立于 IsOS setup 和图版本。

## 兼容的 0.x 图读取器

[P05 读取器特性契约](reader-features.md)在 G05 身份上增加精确的 `supported_node_types` 和 `features`，并增加所选 `egia --capabilities` 运行时探针。这些 EGIA 字段及不启动运行时的探针已经实现；IsOS 准入与 EOS Gateway 就绪仍是独立关卡。仅有图信封版本不能使受管 EGIA 启动就绪。

保留 schema 1 的 `setup` envelope、操作和旧版 identity 标量。Identity 另返回 `supported_graph_versions: ["0.1.0", "1.0.0"]` 和 `supported_node_type_versions: ["0.1", "1.0"]`；旧版 `graph_version: "1.0.0"`、`node_type_version: "1.0"`、`setup_schema_version: 1` 不变。数组存在时以数组为准，旧 adapter 可以不提供数组。图版本数组仅说明可读取的 **envelope**，不代表允许其中任意节点类型。

两个图版本中，`catalog`、`validate` 和 `render` 保留四个已注册的旧 EGIA 设计类型：`egia.common.source@v1.0` 和 `egia.common.context@v1.0` 位于 `egia/common/data`；`egia.common.planner@v1.0` 和 `egia.common.guard@v1.0` 位于 `egia/common/process`。这些设计节点不会获得运行时路由，也不会变成已实现的 EGIA 阶段。[七种精确的 `@v0.1` 阶段类型](pipeline.md)要求完整图、非空实例说明、精确的有类型边，以及与 IsOS 验证端点一致的 link gateway。其 managed 渲染增加 `egia/stages.json`；当前 IsOS reader 仍需独立的类型目录更新，Apply/Run 才可用。`catalog` 返回现有 `egia.agent` profile、默认值、`stage_types` 阶段描述和 `data_types` 对象 schema。每个阶段端口声明全局 `egia.stage.<kind>@v0.1` 数据类型、`in_process` 传输、数据类别及固定路由状态；输入还声明基数和图必需状态。IsOS 在 Apply 或启动前检查实际保存的图和节点 ID，并负责修订检查、迁移和保存字节。EGIA reader 不写入或规范化图文件。

`catalog` 检查请求信封及精确的 EGIA 类型 ID 和放置位置，然后返回所属方描述，不组合图边。这使 IsOS 能在自身准入流程中报告精确的源边诊断。EGIA `validate` 和 `render` 仍检查完整阶段组合、设置、固定路由及已解析网关。

| Identity 字段 | 类型 / 单位 | 含义与验证 |
|---|---|---|
| `supported_graph_versions` | 字符串数组 / 语义版本 | 此 reader 只接受 `0.1.0` 和 `1.0.0`；其他版本在 `/graph/version` 拒绝。 |
| `supported_node_type_versions` | 字符串数组 / 节点 ID 后缀版本 | 支持 `0.1` 阶段和旧版 `1.0` 设计 ID；必须匹配精确的注册身份及位置，后缀本身不授权路径。 |

未知图版本返回 `ok:false`、`files:[]`、退出码 2，并在 `/graph/version` 返回 `unsupported_graph_version`。未知或尚未实现的节点类型在 `/graph/cores/egia/layers/<layer>/groups/<group>/nodes/<node>/node_type` 返回 `unsupported_node_type_version`；已知类型位置错误时在相同路径返回 `invalid_node_placement`。JSON Pointer 路径段对 `~` 和 `/` 转义。缺少 `eos_gateway` 仍是字段级错误。包 manifest 和可执行文件路径由 IsOS 验证。

Managed 配置流程中，IsOS 先把完整已安装 EGIA 默认文件（包括 `models.toml`、`prompts.toml`、`safety/gate.toml`）复制到私有修订目录，再原子应用适配器经验证的覆盖文件。EGIA 启动时仍要求这些文件。`render` 仍只返回相对路径和内容；不接受秘密值或任意绝对路径。IsOS 将不可变修订目录作为 `EGIA_ETC`，只有在兼容 EOS gRPC gateway ready 后才启动 EGIA。完成必需发现前打开监听，不代表 ready。

系统安装前缀为 `<prefix>` 时，EGIA 的工厂回退目录恰好是 `<prefix>/share/defaults/egia`（TOML 文件目录）。IsOS 在 Apply 前选择并清点同一个目录，并在启动前复查。目录缺失时配置加载失败；EGIA 不会退回 `<prefix>/share/island/defaults/egia` 中的资源副本。源码和便携布局继续使用所选共享资源根目录下的 `defaults/egia`。

2026-11-04 的安装布局测试运行了 `uv run ruff format --check .`、`uv run ruff check .`、`uv run pytest -q`（940 个通过）、`uv run pyright`（零错误）、`PYTHONPATH=src:libexec uv run lint-imports` 和 `./bin/islandctl validate`。全新前缀的 wheel/CMake 安装随后通过系统回退目录加载配置，并通过 `egia-config` identity、`egia --capabilities` 与 `islandctl validate`。分阶段安装测试比较工厂文件字节，并在工厂目录缺失时拒绝加载。完整本地输出位于 `logs/test/20261104-installed-defaults/`。

2026-11-01 的 reader 测试使用 `uv sync --extra dev --extra vec`、`uv run ruff format --check src/egia/config_cli.py tests/egia/test_config_cli.py`、`uv run ruff check .`、`uv run pytest tests/egia/test_config_cli.py`（18 个通过）、`uv run pytest`（810 个通过）、`uv run pyright`（零错误）、`PYTHONPATH=src:libexec uv run lint-imports`（六项契约保持）和 `./bin/islandctl validate`，均通过。中英文 `eos-v2.md` 指南从 EGIA 配套源码恢复；完整测试集覆盖其 JSON 示例，相对链接已检查。完整输出保存在本地忽略的 `logs/test/20261101-egia-reader-compat/`。
