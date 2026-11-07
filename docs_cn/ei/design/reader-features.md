# 所选 EGIA 读取器预检

[English](../../../docs/ei/design/reader-features.md) · [归属契约](../interfaces/reader-features.md) · [首次使用](../develop/build-and-run.md)

**已实现的 EGIA 归属切片。** P05 增加不启动运行时的探针与精确 setup 读取器集合。IsOS 根据保存图、已验证生成版本和所选文件决定部署准入。EGIA 只持有真实探针回复和现有 setup 校验。

`egia-config` 是图读取器。其 schema-1 `identity` 应从 Validate／Render 使用的同一 `LEGACY_EGIA_TYPES` 注册表取得四个精确支持 ID、排序，并声明已实现适配器操作的特性常量。不能因接受 `0.1.0` 图信封就声称支持未来的 `@v0.1` 阶段目录。四个旧版 common 节点仍只是设计节点，不组合或执行 A01 流水线。

所选 `egia` 启动器读取配置并连接 EOS，不读取图。在选择 TUI 和导入运行时之前处理 `--capabilities`，且探测路径不得运行 `uv sync` 或初始化虚拟环境。只报告 `binary`、产品版本及 `managed-config-v1` 与 `eos-gateway-client-v2`。后者描述按现有 Gateway 和 CapabilityService 身份编译的客户端；R02 将验证 EOS 服务端和就绪屏障。监听器或 PID 不满足该屏障。

源码检出的包装器可通过 `EGIA_PYTHON`、`ISLAND_PYTHON` 或 `.venv/bin/python` 解析解释器。已实现的探针在检出目录自动安装依赖之前退出，通过 `python -B` 转交仅使用标准库的模块，不导入 `egia.main` 或写入字节码。Setup 包装器使用相同的源码布局判断，并在安装布局中遵循 `ISLAND_LIB`，因此所选 setup 与 runtime 探针从同一模块根目录导入 EGIA。IsOS 必须把探针绑定到启动时实际使用的解释器及安装代码，并将这些输入纳入 P04 校验。Apply 后解释器或环境选择变化会在停止活动子进程前使生成版本失效。仅启动器路径不足以证明包装器所选 Python 实现。

[夹具](../../../ei/tests/fixtures/reader_features/README.md)冻结探针正例、排序后的精确 ID、旧版标量回退、未知阶段／位置失败、运行时特性负例和输入变化时不停止。EGIA 实现从同一所选检出目录执行真实 `egia-config setup --json` identity 与 `egia --capabilities`，并与夹具比较。子进程测试覆盖缺少默认解释器且不运行 `uv sync`、显式所选解释器、模块代码变化的安装前缀，以及无效探针参数。IsOS 持有自己的 stub 探针与停止活动进程前回放。P05 只关闭版本／特性预检边界，R02 就绪与 A01 组合留在各自里程碑。

2026-11-01 设计基线：对两个夹具 JSON 文件分别运行 `python3 -m json.tool`、`python3 tests/fixtures/reader_features/check_cases.py`（22 个唯一用例）、夹具检查器的 Ruff 检查／格式、`git diff --cached --check` 和 14 个改动中英文 Markdown 文件的本地链接扫描均通过。最终输出位于忽略 Git 的 `logs/test/20261101-p05-egia-design/docs-fixtures-final.log`。这些检查只验证契约草案，不证明运行时能力行为。

在工作树本地环境执行 `uv sync --extra dev --extra vec` 后，仓库关卡也全部通过：`uv run ruff check .`、`uv run pytest`（810 项通过）、`uv run pyright`（零错误）、`PYTHONPATH=src:libexec uv run lint-imports` 和 `./bin/islandctl validate`。完整输出位于同一日志目录。最初使用另一检出目录的虚拟环境运行 pytest，产生八项检出路径断言失败；改用本地环境后 810 项全部通过。

2026-11-01 的实现证据保存在忽略 Git 的 `logs/test/20261101-p05-egia-reader-features/`：`uv run ruff check .` 通过，`uv run ruff format --check src/egia/config_cli.py src/egia/reader_features.py tests/egia/test_config_cli.py tests/system/test_reader_features.py` 通过，`uv run pytest` 的 817 项测试通过，`uv run pyright` 零错误，`PYTHONPATH=src:libexec uv run lint-imports` 保持六项契约，`./bin/islandctl validate` 通过。`python3 tests/fixtures/reader_features/check_cases.py` 验证全部 22 个用例标识；两个夹具 JSON 文件均经 `python3 -m json.tool` 解析。聚焦测试回放通过 25 项。这些结果验证 EGIA 的读取器回复和不启动运行时的探针，不证明 IsOS 准入或 EOS gRPC 就绪。
