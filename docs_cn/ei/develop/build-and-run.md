# 在 IsOS 中构建与运行 EI：首次使用顺序

[English](../../../docs/ei/develop/build-and-run.md) · [架构](../design/architecture.md) · [配置接口](../interfaces/isos-config.md) · [TODO](../todo.md)

本指南按首次使用者遇到系统的顺序组织。**当前**独立运行命令可在本 checkout 中连接兼容且已独立运行的 EOS gRPC 服务。IsOS 阶段读取器现已识别七个精确的 EGIA 阶段类型，并可生成托管阶段覆盖文件；所选端到端部署仍是 0.x TODO 中的资格检查关卡。EGIA `0.1.0` 仍以 Python 为运行时。

**当前统一仓库（0.2.1 源码，尚未发布）：** 在 `ei/` 运行 `uv sync --extra dev --extra vec --python 3.12`、`./bin/islandctl validate`，独立启动使用 `./bin/ei --no-tui`。EOS 二进制与契约位于仓库根目录，IsOS manager 源码在 `crates/isos/`，IsosUI 在 `ui/`。托管图 `0.2.0` 只有一个 `isos` 核心；IsOS 将其中的 `ei_*` 层和源修订号交给 setup schema 2 的 `ei-config setup --json`。旧 `egia` 命令、setup 角色、配置目录和线协议标识作为兼容输入保留。下方注明日期的资格证据记录的是此前多仓库开发状态。

## 1. 准备运行环境

安装 Python 3.12 和 `uv`，准备实现 Gateway 发现及 CapabilityService 的 EOS 服务，以及已配置的 OpenAI 兼容模型端点。在本仓库运行 `uv sync --python 3.12` 和 `./egia-build.sh`。`./bin/islandctl validate` 检查当前 checkout 的共享资源及 launcher 路径。当前 README 为独立运行固定了一个开发 EOS 提交；公开配套发布必须针对带标签的 EOS 做真实进程兼容测试。Rust EOS gRPC gateway 已在所选 gRPC 端点提供；HTTP control/catalog 端口是独立接口。

## 2. 配置且不泄露秘密

把 `.env.example` 复制为 `.env`，只设置已选模型 binding 的 `api_key_env` 所指的秘密。在 `etc/egia/models.toml` 中填写真实模型端点，并保持 fast controller 与 slow reasoner 是不同的 binding。在 `etc/egia/profiles/<mode>.toml` 选择 EOS 连接；`etc/egia/modes.toml` 选择默认 mode。`etc/egia/egia.toml` 控制监听、模型选择、阶段策略和日志；`prompts.toml` 与 `safety/gate.toml` 仍是必需配置。启动前运行 `./bin/islandctl validate`、`uv run python scripts/print_prompt.py`；模型端点已启动时再运行 `uv run python scripts/test_model.py`。

**目标 managed 路径**中，IsOS 在私有配置根目录保存编写的 `0.1.0` 图，解析精确包版本及 EOS gRPC 端点，填充完整 EGIA 默认文件，并把与修订绑定的生成配置写入不可变部署修订目录。图保存稳定 ID，不保存秘密或已解析的可执行文件、模型、场景路径。IsOS 调用 `egia-config setup --json` 获取 identity、catalog、validate 和 render；缺少 `eos_gateway`、图/类型版本不支持、模型 binding 无法解析时，Apply/Run 以字段级诊断失败。旧 `1.0.0` 图仍可读取，只能通过明确且带修订检查的 Apply 迁移。

**当前 EGIA reader 检查：** 向 `./bin/egia-config setup --json` 输入 `{"schema_version":1,"op":"identity","core":"egia"}`。响应列出图 envelope `0.1.0` 和 `1.0.0`、四个旧 common 设计 ID，以及七个精确的 `@v0.1` 阶段 ID。完整新图可验证并渲染 `egia/stages.json`；不完整图、冲突的 EOS gateway、图编写的命令路径或未知类型会在图路径报错。已合并的 IsOS A01 读取器准入这些精确阶段 ID，供 Apply/Run 使用。本机 identity 检查不启动 EGIA，也不改变保存的图字节；示例和诊断见 [setup 接口](../interfaces/isos-config.md)。

**当前 EGIA P05 读取器检查：** 查询同一个所选安装的 `egia-config` identity 和 `egia --capabilities`，比较[读取器契约](../interfaces/reader-features.md)中的精确 ID 集合与特性集合。对源码检出运行 `printf '%s' '{"schema_version":1,"op":"identity","core":"egia"}' | ./bin/egia-config setup --json` 和 `./bin/egia --capabilities`。运行时探针不初始化虚拟环境，也不启动 EGIA。所选解释器缺失时以 `127` 退出；安装环境或把 `EGIA_PYTHON` 指向预期解释器。IsOS P05 后续将用保存图路径和所选读取器报告不支持的精确节点或特性；恢复已验证的兼容安装或修正图，然后重新 Apply。探针成功本身不证明 EOS gRPC 就绪。

## 3. 按依赖顺序启动

**当前独立运行**时，先启动兼容 EOS 与模型服务，再运行 `./egia-launch.sh --no-tui` 或 `./bin/egia --no-tui --mode <mode>`。`--etc` 选择其他 EGIA 配置根目录；`--island-config` 选择共享资源。北向监听默认是 `127.0.0.1:16060`。如果 EOS 发现或必需 schema 失败，即使 TCP 监听已打开，也应将 EGIA 视为不可用。

**目标 managed 部署**先运行 `isos version check`、`isos config validate <graph.json>`、`isos config apply <graph.json>`，再运行 `isos run`，或在 IsosUI 中使用 Save and Run。IsOS 启动 workbench、解析包、启动 EOS 及其 MuJoCo 子进程、验证 EOS gRPC ready，然后以 `EGIA_ETC=<generated-root> --mode isos --managed --no-tui --state-root <isos-data-root>/state/<deployment>` 启动 EGIA。设置 `ISOS_HOME` 时，IsOS 数据根目录为 `<ISOS_HOME>/data`；否则为 `${XDG_DATA_HOME:-~/.local/share}/isos`。IsOS 为同一部署的 Run/Stop/Restart 提供这个部署专属的可变根目录；EGIA 拥有其中 `egia/` 子目录的文件。该根目录与图及按修订生成的配置分离。IsOS 管理重启与逆序停止。`isos deployment stop` 只停部署，保留 workbench。IsOS 模型服务仍是独立 service，在共享状态和日志中显示，而不是 EGIA 图节点。

**A02 托管控制检查：**IsOS 还传入 `EGIA_EOS_CONTROL_GRANT_FILE`，值为指向本次运行 EGIA grant 的绝对路径，文件权限为 `0600`。不要把 grant 字节复制到图、日志或命令行。所选 `egia/stages.json` 必须用 64 位小写 SHA-256 携带 IsOS 保存图 revision。只有 Discover、GetStatus 和 ListCapabilities 返回一致的 `eos-active-revision` 与 `eos-projection-sha256`，EGIA 才接受 EOS 写入就绪；前者还必须等于该所选覆盖文件的 revision。发现代际混杂、内部一致但属于另一张图，或 grant 不安全时，检查所选运行并修正所有者输入后从 IsOS 重启。Submit/Watch 响应丢失时，用同一个 ID 和 grant 通过 GetOperation 核对。若没有匹配的终态 acknowledgement，应把物理结果视为未知，并使用所有者状态／停止流程；绝不能为同一运动生成新 ID。


**A03 恢复与本机重置：** 每个获准原语在 Submit 前持久预留已验证的操作 ID。重启仅用该 ID 查询 `GetOperation`；缺失收据保持未解决并阻止安全重置。全机锁存在紧急外发前 `fsync`，跨 EOS 和 EGIA 重启保留。外部处理 EOS estop 后，停止 EGIA，在同 UID 的本机交互终端运行 `./bin/egia safety reset --state-root <isos-data-root>/state/<deployment> --episode <episode-id> --graph-revision <selected-sha256>`（IsOS 托管部署）。将 `<isos-data-root>` 替换为上述当前 IsOS 数据根目录，传入该部署使用的精确包含状态根目录，不是其 `egia/` 子目录；独立启动则使用当时启动 EGIA 的根目录。命令只在运行时已停止且受影响别名精确匹配时写入一次性授权。下一次启动消费授权，对每个已保存的别名与 robot ID 做两次所选修订的完整 EOS 检查；只有 estop 为 false、状态安全且没有未解决操作才持久解锁。检查失败保持锁存；管理器、HTTP 和 WebSocket 均无重置路径。

## 4. 按因果顺序读取状态和日志

目标集成中运行 `isos status --json`，打开 IsosUI workbench 的日志面板。第一次运行按“包/版本兼容 → 图/配置修订 → EOS 子进程和 gRPC ready → EGIA 发现代际 → EGIA ready → Mission/提案/gate/操作结果 → 停止”的顺序阅读。先按 `run_id`，再按 `correlation_id`，最后按 `node_path` 筛选。监听或 PID 只能证明进程启动，不能证明发现与安全初始化成功。动作被拒绝时，从 gate 事件码及缺失/过期的精确绑定开始排查，不要盲目重试。

[EGIA 事件契约](../interfaces/events.md)固定共享 JSONL 信封和 24 个允许的 `egia` 事件码。IsOS 传入部署 `ISOS_RUN_ID=r...`；托管 EGIA 在写共享记录前验证它。绑定缺失或错误时不会发 EGIA 事件，由 IsOS 报告所选子进程退出。独立 EGIA 生成本地 `l...` ID。托管 stderr 只承载脱敏 JSONL；原生 stderr 写入被丢弃。轮转的私有归属 replay（32 MiB、五个备份）位于所选状态根目录下的 `egia/egia.jsonl`；人类诊断使用 EGIA 解析后的日志根目录。源码独立启动保留现有的 `var/lib/egia` 与 `var/log/egia` 默认位置。完整 EOS 发现、模型绑定和安全初始化后才读取 `egia_ready`。EOS Submit 响应丢失时运动可能已获准入；遇到 `egia_operation_unknown` 和 `egia_recovery_required`，先查询状态／收据并核对，绝不重放运动。收集器故障不能阻塞停止或替代归属 replay；重复的非关键诊断计数，首个故障和终结事件不抑制。IsOS 所选子进程边界回放仍是单独的集成门槛。

## 5. 恢复与停止

版本不匹配时检查 `isos version check`、已保存图版本、类型 ID 和精确包 pin。配置失败时修正 `egia-config` 报告的字段，不编辑生成文件。EOS 发现失败时检查 gRPC 端点和 EOS 节点状态；EOS HTTP 16020/16021 不是 gRPC。模型失败时检查 binding 名称、秘密环境变量、端点和 decision 事件。Gate 拒绝时先检查能力身份和安全策略，再考虑新尝试。紧急锁存只能在 EOS 和场景确认安全后通过授权流程重置。

目标部署用 `isos deployment stop` 或附着的 `isos run` 终端 Ctrl+C 停止；`isos stop` 停止整个 workbench。独立 EGIA 用前台终端中断。正常停止先拒绝新提案，按 EOS 终结操作状态完成或取消在途工作，保存 Mission 结果，关闭 link，并产生一个终结运行事件。超时、进程退出和 socket 关闭都不能解释为物理成功。

## Q03 所有者资格回放

[Q03 设计](../design/q03-qualification.md)规定所选 Q01/Q02 前提、安装 wheel 检查门、故障与 Stop 证据及两个基准负载。下文记录所有者证据及已完成的安装进程检查门；这些命令用于新的资格运行。使用全新 `<run-id>` 和隔离检出；运行前固定所有者精确提交及所选图。顺序运行完整测试时，确保没有其他 EGIA 子进程占用 `127.0.0.1:16060`。

```sh
uv sync --extra dev --extra vec
uv run ruff format --check .
uv run ruff check .
uv run pytest -q tests/egia/test_stages.py tests/egia/test_stage_launch.py tests/egia/test_vision_evidence.py tests/egia/test_link_feedback.py tests/egia/test_a03_process_restart.py tests/egia/test_shared_events.py tests/egia/test_recovery_events.py
uv run pytest -q
uv run pyright
PYTHONPATH=src:libexec uv run lint-imports
./bin/islandctl validate
python3 tests/fixtures/q01_vision_evidence/check_cases.py
python3 tests/fixtures/jsonl_events/check_cases.py
git diff --check
```

每条命令以 `set -o pipefail` 和 `tee logs/test/<run-id>/<gate>.log` 保存完整标准输出/错误，并记录退出码；保留首次失败及重跑原因。在固定提交的 EGIA 检出中，以干净 shell 启动私有安装探针；使用唯一前缀并归档其绝对路径：

```sh
egia_q03_prefix="$(mktemp -d "${HOME}/egia-q03.XXXXXXXX")"
uv build --wheel --out-dir "$egia_q03_prefix/wheels"
uv venv --python 3.12 "$egia_q03_prefix/.venv"
uv pip install --python "$egia_q03_prefix/.venv/bin/python" "$egia_q03_prefix"/wheels/egia-0.1.0-*.whl
cmake -S . -B "$egia_q03_prefix/cmake-build" -DCMAKE_INSTALL_PREFIX="$egia_q03_prefix" -DISLAND_INSTALL_ETCDIR="$egia_q03_prefix/etc/island" -DISLAND_BUILD_EOS=OFF
cmake --install "$egia_q03_prefix/cmake-build"
cd "$egia_q03_prefix"
unset PYTHONPATH ISLAND_LIB ISLAND_LIBEXEC ISLAND_PYTHON
mkdir -p "$egia_q03_prefix/isos-home"
export ISOS_HOME="$egia_q03_prefix/isos-home"
export EGIA_PYTHON="$egia_q03_prefix/.venv/bin/python" ISLAND_PREFIX="$egia_q03_prefix" ISLAND_MODE=system
export ISLAND_ETC="$egia_q03_prefix/etc/island" ISLAND_SHARE="$egia_q03_prefix/share/island"
export ISLAND_CONFIG="$egia_q03_prefix/etc/island/island.toml" EGIA_ETC="$egia_q03_prefix/etc/island/egia"
./bin/islandctl paths
./bin/islandctl validate
printf '%s' '{"schema_version":1,"op":"identity","core":"egia"}' | ./bin/egia-config setup --json
./bin/egia --capabilities
"$EGIA_PYTHON" - <<'PY'
from pathlib import Path
import egia, island
from island.path import Layout
from island.share import configured_resources
prefix = Path.cwd().resolve()
layout = Layout.resolve()
resources = configured_resources(layout)
assert layout.egia_defaults == prefix / 'share/defaults/egia'
assert (layout.egia_defaults / 'egia.toml').is_file()
selected = [Path(egia.__file__), Path(island.__file__), layout.etc / 'island.toml', layout.share, layout.egia_defaults, *resources.roots, *resources.files.values()]
assert all(path.resolve().is_relative_to(prefix) for path in selected), selected
print('installed paths:', *(str(path.resolve()) for path in selected[:4]), sep='\n')
PY
```

明确设置 `ISLAND_INSTALL_ETCDIR` 可阻止 CMake 默认绝对路径 `/etc/island` 越出前缀。核对 wheel 模块路径及每个所选共享资源文件都在前缀内；针对所选保存图运行 setup catalog/validate/render，并比较精确类型/特性 ID 及 `stages.json` 摘要。然后用所有者验证器回放所选全新 home 的 IsOS → EOS Rust gRPC → MuJoCo 0.1.1 → EGIA 流程。保存构建/安装命令、前缀、`ISOS_HOME`、wheel 摘要、所选修订、操作/终结收据、故障/重启/Stop、环境清单和完整日志。基准 runner 位于 `tests/benchmarks/`；以同一运行 ID 归档精确调用、五轮原始样本、中位数/p99 及环境。`logs/test/` 被忽略跟踪，还需向评审者提供持久证据产物。

### Q03 所有者测量，2026-11-03

固定负载 runner 和计数测试见 [tests/benchmarks/README.md](../../../ei/tests/benchmarks/README.md)。本切片执行 `uv run pytest -q`（938 项通过）、`uv run pyright`（零错误）、`uv run ruff check .`（通过）、六项导入契约、`islandctl validate` 和两套冻结夹具检查。阶段/Vision/链路/恢复及基准测试的聚焦集 120/120 通过。首次 `uv run ruff format --check .` 报告本切片以外原有 67 个文件需要格式化；完整失败日志已保留。单独的纯格式化 PR #102 与后续精确提交 CI 已通过全库格式检查。私有前缀中的 `egia==0.1.0` wheel（SHA-256 `777ae8ee40d2ca06cedbfc4277b1ec95f8591976d07747e684e8f0015102178b`）与 CMake 安装通过，导入路径未落回源码。已安装的 setup 使用所选 Q01 保存图完成 catalog/validate/render；生成的 `egia/stages.json` 与归档文件逐字节一致（SHA-256 `1e239085ec48ac988cdc30732e7843141a184ecfd60ca6e3b49459a971a2a702`，修订 `1964bdae4a0291b17306f9861bc971e158cebac59a115a946658e94fa6a88a89`）。

测量使用源码提交 `e5e05a71772ad43899014fdddfc6e38ebf8a9ec7` 及阶段 runner SHA-256 `0d0889de2f545f6bd9def307ab00b20627d39d0fb1f4305b41aa6d394f7f12b0`（Vision 测量使用先前的 runner SHA-256 `9798d66860333c44513f3a4e02990eb3528913367f8ab0ceb8ed7ad4ff8856a4`），CPython 3.12.14、uv 0.12.5、Linux 7.0.0-38-generic、Intel i5-12490F（6 核/12 线程）、15 GiB 内存、CPU 亲和范围 0–11、`powersave` 调速器、无容器 cgroup，且没有主动并行的基准负载。每个负载预热一轮、测量五轮，时钟为 `time.monotonic_ns`。实际发布的 resource 载荷为 92 字节（SHA-256 `e389fc4fa4bda35e7565b6d2d42b6c8b1625c1ba9935b4bd4f8f374f654d1955`），独立发布的 observations 载荷为 12 字节（SHA-256 `eef46741adfc3a9f76294d3b78f37a45f113092ac9d44ee77c7a038a88ff09a1`）；Vision JPEG 为 28 字节（SHA-256 `32f562ab852a38f44256a63190050b77fac48cda760a6bda06dc6c2ca4d8d283`）。这些结果受投送速率约束，仅构成初始基线，不表示最大吞吐量或发布目标。

| 负载 | 吞吐量中位数（五轮范围） | 每轮送达 / 覆盖 | 延迟中位数 / p99 |
| --- | --- | --- | --- |
| 阶段 resource：10,000 项、1,000/s | 1,000.092/s（1,000.079–1,000.092） | 10,000 / 0 | 0.027096 / 0.169410 ms |
| 竞争 resource：10,000 项、1,000/s、30 Hz 上限 | 28.273/s（28.272–28.275）；合计 999.132/s（999.099–999.215） | 283 / 9,717 | 0.028513 / 0.262283 ms |
| 竞争 observations：10,000 项、1,000/s、优先级 -10 | 970.859/s（970.827–970.940） | 9,718 / 282 | 0.012717 / 0.091878 ms |
| Vision 所有者 UDS：1,000 项、30 fps | 30.029/s（30.028–30.029） | 接受 1,000 / `generation_stale` 拒绝 10 | 0.394629 / 1.581212 ms |

完整源码检查输出、已安装前缀记录、所选渲染回应、环境清单及 Vision 样本位于被 Git 忽略的 `logs/test/20261103-q03-egia/`；评审修复后的阶段重跑样本位于 `logs/test/20261103-q03-egia-review-fix/`；删除 worktree 前应复制到持久评审产物。这是所有者测量及已安装 setup 检查。已跟踪的 CI 安装夹具检查要求所选 Q01 EGIA 覆盖文件逐字节一致、七个阶段 ID 和两条路由。在 EGIA 提交 `d867ee36599f2ff74182f5a335ed295354d8318e` 上，[push 作业](../../island/retired-repository-references_cn.md#ref-011d07ee0935)与 [PR 作业](../../island/retired-repository-references_cn.md#ref-b21d99484e1e)的源码和安装检查门均通过。push 作业记录 938 项测试通过、Pyright 零错误、六项导入契约、Ruff 格式与 lint 通过、两套冻结夹具检查、源码与安装的 `islandctl validate`，以及 [CI 产物](../../island/retired-repository-references_cn.md#ref-e00595258e13)中的安装 setup 收据。wheel SHA-256 为 `9ab7cee096aafb5a16e764d3dc32aef05b703a02162e6af890dc6b3a00faf6b6`；安装的 `stages.json` 摘要与 `1e239085ec48ac988cdc30732e7843141a184ecfd60ca6e3b49459a971a2a702` 一致。完整作业日志及下载的收据保存在本地忽略的 `logs/test/20261103-q03-egia-ci-fix/run-41543538334/` 和 `run-41543546782/`。已合并的 [PR #103](../../island/retired-repository-references_cn.md#ref-bd5748ad94f4) 与 [PR #104](../../island/retired-repository-references_cn.md#ref-df85274ba886) 之后，精确选定的 EGIA 提交 `2e7abae02d6538ba9e2519d6a42eb9ca184c34ec` 通过 [push 源码与安装作业](../../island/retired-repository-references_cn.md#ref-075162f7d189)和 [PR 源码与安装作业](../../island/retired-repository-references_cn.md#ref-ef872dabc63c)。[push 安装收据](../../island/retired-repository-references_cn.md#ref-d5d1a1839ee7)与 [PR 安装收据](../../island/retired-repository-references_cn.md#ref-191da7c422b9)可供评审下载，但受 GitHub 保留期限限制。封存的 EOS `logs/test/20261103-q03-selected-final-17/` 验证器对包含 EGIA `2e7abae` 的所选主题分支提交报告 `Q01_RESTART_SCOPED_VERIFIED_PASS` 和 `Q01_FULL_VERIFIED`。另行保存忽略跟踪的本地归档。final17 的 EGIA 真实子进程使用源码检出的 `.venv`；wheel／前缀收据证明已安装 setup。下文补充的安装前缀重放提供托管真实进程证据。EOS Q03 源码对账及 Q04 合并后发布资格验证另行处理。

### 已安装 EGIA 托管进程重放，2026-11-04

[所有者收据](../design/q03-qualification.md#已安装-egia-托管进程证据2026-11-04)固定 EGIA `4db8319c`，其[源码与安装 CI 作业](../../island/retired-repository-references_cn.md#ref-26e039c8e5e4)通过。[EOS PR #347](https://github.com/Island-Team/Island-eos/pull/347) 将重放测试程序与证据以 `573d7b15` 合入 `refactor(v2)/runtime-main`。已安装 wheel 的 SHA-256 为 `c438f76de01a174f7d36599006af5f1442dbb1c6a9c144c51cad274d9b3a09d8`。全新前缀和 IsOS home 下，IsOS 生成 `egia_chain.mode=installed`，随后使用前缀导入路径和 home 当前目录启动 EGIA。两次运行达到七阶段并完成受治理的 `base.velocity`；故障注入、旧授权拒绝、`robot.stop`、确认 Stop 和子进程回收通过。在全新 home 的三个负向启动分别于创建子进程前拒绝安装模块篡改、所选覆盖文件篡改及错误解释器；未修改的对照组成功启动并停止。

在 EOS `573d7b15` 检出中保留被忽略的 `logs/test/20261104-q03-egia-installed/` 归档，再运行以下只读检查。原始可执行参数列表在 `live/command-argv.json`（SHA-256 `8e682e7e203a0844c971f100da493c4664f357f1bb3a8f445eb622d041072fdf`）和 `external/negative-command-argv.json`（SHA-256 `87e4c63619076de12a655707bedac6e1798e55e2287930f4d6ad70a973d11467`）。构建、临时与输出路径均位于 `/home`；正负证据封印 SHA-256 作为验证器外部输入。

```sh
q03_eos_archive=logs/test/20261104-q03-egia-installed
python3 tests/verify_q03_egia_installed.py "$q03_eos_archive/live" "$q03_eos_archive/external/inputs.json" \
  --expect-inputs-sha256 3688ba488a91192e5b16b00cba1dae18a8a80e6578e2edad9794616c93b2dbee \
  --expect-evidence-sha256 07dad1b546c58f3683798d5fbd225165c8052c2753b6c4f6b15e2d875759367b
python3 tests/verify_q03_egia_installed_negatives.py "$q03_eos_archive/negative-4" "$q03_eos_archive/live" "$q03_eos_archive/external/inputs.json" \
  --expect-inputs-sha256 3688ba488a91192e5b16b00cba1dae18a8a80e6578e2edad9794616c93b2dbee \
  --expect-positive-sha256 07dad1b546c58f3683798d5fbd225165c8052c2753b6c4f6b15e2d875759367b \
  --expect-evidence-sha256 5e179082da501496e197171bf61a8403b3d0083f0e8295310b08f39583504cd1
```

独立评审对复制后的归档运行两个验证器均通过。较早的 Q04 final-5 选定归档仍提供完整浏览器与迁移矩阵；本次重放专门证明已安装 EGIA 托管进程边界，不表示发布标签。

## 开发检查

仅修改文档时检查链接、中英文对应和所有示例语法。实现时运行 `uv run ruff check .`、`uv run pytest`、`uv run pyright`、`PYTHONPATH=src:libexec uv run lint-imports`、`./bin/islandctl validate`，以及 IsOS → EOS → MuJoCo → EGIA 的真实进程启停集成。完整输出保存在 `logs/test/<run-id>/`，结果摘要写入相关设计页。

此 checkout 已提供 P06 生产者；只在所选 `--managed` 启动中使用 `ISOS_RUN_ID=r<run>`。直接诊断检查可运行 `uv run pytest -q tests/egia/test_shared_events.py`。按 `run_id` 核对首条 `egia_starting`、任何 `egia_start_failed` 和最终停止；未确认停止为 error。IsOS 聚合与游标导航仍需独立集成回放。

2026-11-02 的 A01 EGIA 阶段检查运行了 `uv run ruff check .`、`uv run pytest`（849 项通过）、`uv run pyright`（零错误）、`PYTHONPATH=src:libexec uv run lint-imports`（六项契约通过）和 `./bin/islandctl validate`（通过）。完整输出保存在本地忽略的 `logs/test/20261102-egia-a01/`。`tests/egia/test_stage_launch.py` 使用 EOS stub 检查托管七阶段覆盖文件、有类型 resource → perception → context → decision 传递，以及停止时不捏造结果。IsOS reader 准入这些新 ID 和完整 IsOS → EOS → EGIA 运行仍属于独立集成工作。

A01 独立审查要求在重复记录前检查同一 run 的事件年龄和 resource sample。2026-11-02 后续检查使用相同的五道门，全部通过，包括 855 项测试和零 Pyright 错误。完整后续输出位于 `logs/test/20261102-egia-a01-review-fix/`；假时钟及错误／空 resource 测试覆盖了修正的边界。目录测试还验证 IsOS 可在 EGIA Validate/Render 之前诊断源边 QoS。

2026-11-02 的 A04 所有者回放运行了 `uv run ruff check .`、`uv run pytest`（869 项通过）、`uv run pyright`（在 `uv sync --extra dev --extra vec` 后零错误）、`PYTHONPATH=src:libexec uv run lint-imports`（六项契约通过）及 `./bin/islandctl validate`（通过）。完整输出保存在本地忽略的 `logs/test/20261102-egia-a04/`。`tests/egia/test_stages.py` 用假 UTC 和单调时钟检查精确 QoS 准入、极端频率、加权竞争、最新值溢出与缺口计数、所有者过载和停止屏障；`tests/egia/test_stage_launch.py` 检查托管 stub 路径。首次使用时，在运行中过载时查看 `stage_edge_overflow`，停止时查看 `stage_edge_delivery`，并确认停止后没有待传证据。所选 setup 身份与运行时能力探针分别声明 A04；IsOS G06 尚未注册这些 EGIA 路径，带边 QoS 的 IsOS Apply/Run 仍是独立集成关卡。

2026-11-02 的 A02 link 检查运行了 `uv run ruff check .`、`uv run pytest`（880 项通过）、`uv run pyright`（安装可选 `vec` extra 后零错误）、`PYTHONPATH=src:libexec uv run lint-imports`（六项契约通过）与 `./bin/islandctl validate`（通过）。完整输出位于本地忽略的 `logs/test/20261102-egia-a02/`。`tests/egia/test_link_feedback.py` 覆盖混杂／重复的发现 metadata、不安全 grant 文件、过期 revision 与错误运行的拒绝、stop/estop metadata，以及 Submit/Watch 丢失后用同 ID 查询且不第二次调用 adapter。

A02 独立审查发现所选 revision 缺口。后续检查增加严格的 64 位小写 SHA-256 `stages.json` 读取器，把所选 revision 经阶段组合传给每个托管 EOS client，并在首次或强制发现时拒绝内部一致但属于另一张图的 EOS 代际。`uv run ruff check .`、`uv run pytest`（885 项通过）、`uv run pyright`（零错误）、`PYTHONPATH=src:libexec uv run lint-imports`（六项契约通过）、`./bin/islandctl validate` 和 `git diff --check` 全部通过。完整输出位于本地忽略的 `logs/test/20261102-egia-a02-review-fix/`。

所选 A02 真实进程回放使用 IsOS `f1fc0d7`、EOS runtime-main `c3aec924` 和 EGIA `9c70560`，在干净的临时 home 中加载固定版本的 `island/mujoco-sim@0.1.1` pick-place 场景。IsOS Validate/Apply 写入保存的 revision `be27e580504d78af79707415836ff4eba5f039483c0211a966719f192c290c3f`，所选 `egia/stages.json` 带有相同 revision。受监管的 EGIA 只收到仅所有者可读的 grant 文件路径，为 run `r820736-18e421645b4183d4-1` 发出 `egia_ready`，并发现 EOS 的三个设备和四个 primitive。经准入的 `base.velocity` 操作 `a02-live-move-1793595554910856244` 和 `robot.stop` 操作 `a02-live-stop-1793595555209368058` 都返回已接受的终态 `succeeded`。IsOS Stop 后，强制重新发现阻止后续写入；IsOS 确认按 EGIA → config → bridge → EOS 顺序停止，并回收四个进程。完整命令、所有者事件、状态、操作 ID 和验证器输出位于本地忽略的 `logs/test/20261102-egia-a02-live/`。真实回放脚本在 Stop 后因最后一条断言预期了错误的终态名称而退出；IsOS 正确返回 `confirmed`。修正后的离线验证器核对保存的所有者证据并通过。此结果完成 A02 所选进程交接；完整 M4 与发布验收仍是独立关卡。

A04 审查后的修正为单源数组诊断保留 `/0`。阶段测试检查嵌套 source、output、priority、frequency 的精确指针和不受支持的 QoS。相同五道门再次通过（874 项测试、零 Pyright 错误、六项导入契约）；完整输出在 `logs/test/20261102-egia-a04-review-fix/`。

A03 所有者切片运行了 `uv run ruff check .`（通过）、`uv run pytest`（顺序资格检查 896 项通过）、`uv run pyright`（零错误）、`PYTHONPATH=src:libexec uv run lint-imports`（六项契约通过）、`./bin/islandctl validate`（通过）、`python3 tests/fixtures/jsonl_events/check_cases.py`（24 个精确事件码、53 个案例）和 `git diff origin/master --check`（通过）。完整输出位于本地忽略的 `logs/test/20261102-egia-a03/`。一次并行门禁中的 sandbox 就绪测试受资源竞争超时；单独重跑该模块 5/5 通过，随后完整顺序测试 896/896 通过。`test_recovery_events.py` 覆盖原子且仅所有者可访问的状态、锁与 TTY 重置、一次性授权、错误别名、真实 estop 和未解决操作。`test_link_feedback.py` 验证同一客户端用保存 ID 查询收据且没有第二次 Submit；该测试本身并未重启 EGIA 进程。

聚焦的进程边界证明运行了 `A03_EVIDENCE_DIR=logs/test/20261102-a03-process-restart-proof/raw uv run pytest -q tests/egia/test_a03_process_restart.py`（2 项通过），随后运行 `uv run ruff check .`、`A03_EVIDENCE_DIR=logs/test/20261102-a03-process-restart-proof/raw uv run pytest -q`（907 项通过）、`uv run pyright`（零错误）、`PYTHONPATH=src:libexec uv run lint-imports`、`./bin/islandctl validate`、`python3 tests/fixtures/jsonl_events/check_cases.py` 和 `git diff --check`（均通过）。原始子进程 stdout/stderr 与 stub RPC 身份保存在同一个本地忽略的运行目录。首次完整 pytest 得到 906 项通过，另有一个固定端口冲突：另一个正在运行的所选 EGIA 子进程占用 `127.0.0.1:16060`；该进程停止后，安静环境中重跑全部 907 项通过。测试在两个真实 EGIA 工作解释器之间保持同一个 stub EOS 服务、grant 和模拟 run ID，并检查正式的待决检查点启动循环。它不证明 IsOS 受监管的同一 run 子进程替换。

Q01 所有者状态证据使用新的私有目录运行：`A03_EVIDENCE_DIR=logs/test/<run-id>/raw uv run --extra dev pytest -q tests/egia/test_a03_process_restart.py`。检查[安全重置契约](../interfaces/safety-reset.md)所列五个 `positive-*` 和 `negative-*` 快照清单。2026-11-03 聚焦测试在启用和禁用钩子时均通过 7/7，Ruff、Pyright 通过，五份原始文件清单完成核对；完整输出在本地忽略目录 `logs/test/20261103-q01-a03-owner-state-evidence/`。这仅是测试证据，须经所选 Q01 归档校验器和独立实现审查后才能宣称合格。

### Q01 Vision 所有者读取器

选定受管运行时，IsOS 设置 `EGIA_VISION_BINDING_FILE`、`EGIA_EOS_VISION_GRANT_FILE`、`EGIA_VISION_INSPECTION_SOCKET` 和 `EGIA_VISION_INSPECTION_VERIFIER_FILE`：绑定与授权保留在私有 control-grant 运行根目录，两个套接字及校验文件位于短私有 `<runtime-root>/v/<key>/` 目录。启动前 `bin/egia --capabilities` 须公布两个 Vision 读取器标记。EGIA 用受管运行 ID 与 `stages.json` 版本校验绑定，订阅选定 EOS 所有者 UDS，并仅通过独立授权的检查 UDS 提供元数据。未提供绑定时此可选入口关闭；提供无效绑定时启动失败。停止时先取消订阅并清除 JPEG，再退出进程。Q01 选定回放须使用 IsOS 所有者验证器，对比 EOS 交付身份与 EGIA 检查快照中经验证的摘要和字节长度。

2026-11-03 的短端点兼容检查使用 `TMPDIR=/home/operator/t uv run pytest -q --basetemp=/home/operator/t/pt-egv`，将测试产物保留在较大的 `/home` 文件系统，同时让 Unix 套接字名称不超过内核限制；940/940 个测试通过。`uv run ruff format --check src/egia/vision_evidence.py tests/egia/test_vision_evidence.py`、`uv run ruff check .`、`uv run pyright`（零错误）、`PYTHONPATH=src:libexec uv run lint-imports`、`./bin/islandctl validate` 和 `git diff --check` 均通过。首次使用 `/home/operator/egia-test-tmp` 的 pytest 尝试因更长的基目录超出 Unix 套接字路径限制，导致 20 个已有套接字测试失败；完整失败与成功输出保留在 `logs/test/20261103-vision-short-endpoints/`。聚焦测试验证派生端点身份、校验文件篡改、符号链接拒绝、受管重启与 Stop 清理。跨所有者 Q04 回放仍是发布关卡。

2026-11-03 的 Q01 EGIA 所有者实现运行了 `uv run ruff check .`、`uv run pytest -q`（914 个测试通过）、`uv run pyright`（零错误）、`PYTHONPATH=src:libexec uv run lint-imports`（六项导入契约保留）、`./bin/islandctl validate`、`python3 tests/fixtures/q01_vision_evidence/check_cases.py`（18 个设计用例有效）及 `git diff --check`。完整输出位于忽略跟踪的 `logs/test/20261103-q01-vision-egia/`。聚焦 stub 测试覆盖帧身份与摘要拒绝、同 stream generation、新 stream 握手、独立检查授权、运行时授权与验证器撤销以及受管环境启动。这是 EGIA 所有者关卡；选定 IsOS/EOS/EGIA 回放仍属 Q01 工作。

独立 Q01 Vision 实现审查发现两处帧准入边界：SOF 与声明尺寸须遵守 EOS 的 1–4096 像素限制，且握手证明 stream 前不得接受缺失或 null 的 stream。修复加入超界／布尔尺寸、缺失／null stream 的聚焦反例，并覆盖错误端点、畸形 JPEG、投影版本错误及重连游标。`uv run pytest -q tests/egia/test_vision_evidence.py` 通过 9/9；完整测试首次运行因其他活动进程占用固定 `127.0.0.1:16060` 端口失败，端口释放后通过 916/916。`uv run ruff check .`、`uv run pyright`（零错误）、`PYTHONPATH=src:libexec uv run lint-imports`（六项契约保留）、`./bin/islandctl validate`、18 用例冻结设计检查器及 `git diff --check` 均通过。完整输出（包括首次失败记录）位于忽略跟踪的 `logs/test/20261103-q01-vision-egia-review/`。冻结用例文件仍是设计快照；聚焦测试验证实际行为。选定跨所有者回放仍未完成。

Q01 重复代际首次使用时，在一次性 home 中以继承 `EGIA_Q01_STALE_TEST=1` 的环境启动**新的**所选 IsOS 管理器；先确认没有既有管理器套接字。把带标记的 `isos run` 发给已有的未带标记管理器，不会启用子进程测试入口。IsOS 拒绝在 EGIA 进程图环境中编写此标记。所选 EOS 与 EGIA Vision 检查确认当前 stream、代际、帧 ID 和 JPEG 摘要一致后，依据[私有所有者契约](../interfaces/vision-evidence.md)从当前 EGIA 检查快照构造仅用于比较的请求。向短端点目录中检查套接字旁的 `egia-vision-stale-test.sock` 发送一行，归档原始的仅含元数据请求和响应，以及浏览器和 EGIA 的前后检查快照。只有合法新帧到达并重新取快照后才可重试 `target_changed`。选定响应必须报告 `generation_stale`、仅增加一次拒绝、不增加接受，并保持当前身份与摘要不变。确认一次调用后和停止后测试套接字均不存在。此 EGIA 聚焦测试关卡本身不能充当所选 Q01 真实回放收据。

2026-11-03 的 EGIA 过期测试入口通过 `uv run ruff format --check`、`uv run ruff check .`、`uv run pytest`（923 项通过）、`uv run pyright`（零错误）、`PYTHONPATH=src:libexec uv run lint-imports`（六项契约通过）、`./bin/islandctl validate`、fixture JSON 解析及 `git diff --check`。完整输出位于忽略跟踪的 `logs/test/20261103-q01-vision-stale-test/`。聚焦测试执行真实本地套接字和准入主体；独立实现审查与所选真实回放仍待完成。

独立实现审查发现绑定后启动清理缺口。修复在启动前登记 hook 归属，记录创建的套接字 inode，并在 chmod 或 lstat 失败后关闭监听器且仅删除该 inode。注入测试还证明替换的路径保持不变，受管启动失败会移除两个私有监听器。2026-11-03 相同的完整检查再次通过，共 927 项测试、零 Pyright 错误；完整输出位于 `logs/test/20261103-q01-vision-stale-review-fix/`。独立复审与所选真实回放仍待完成。

绑定后首次路径 stat 故障现在会关闭监听器，但不会删除尚未确定所有权的目录项。测试证明原套接字不再监听，留待运行目录清理；被替换的同 UID Unix 套接字继续监听且不受改动。独立复审否决了此前的回退方案，因为该方案可能删除替换套接字。最终检查证据见下文。

2026-11-03 所有权修复通过 `uv run ruff format --check src/egia/vision_evidence.py tests/egia/test_vision_evidence.py`、`uv run ruff check .`、`uv run pytest -q`（930 项通过）、`uv run pyright`（零错误）、`PYTHONPATH=src:libexec uv run lint-imports`（六项契约保持）、`./bin/islandctl validate`、fixture JSON 解析及 `git diff --check`。完整日志位于忽略提交的 `logs/test/20261103-q01-vision-stale-ownership-fix/`。独立复审仍待完成。
