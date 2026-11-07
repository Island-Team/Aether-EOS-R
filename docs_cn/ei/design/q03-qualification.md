# Q03 EGIA 资格验证设计

[English](../../../docs/ei/design/q03-qualification.md) · [阶段接口](../interfaces/pipeline.md) · [Vision 接口](../interfaces/vision-evidence.md) · [首次使用](../develop/build-and-run.md)

本文是 EOS Q03 与 EGIA E5 的 EGIA 所有者设计，定义资格证据，不增加运行时接口。EGIA 所有者证据包括已合并的[安装与基准切片 #103](../../island/retired-repository-references_cn.md#ref-bd5748ad94f4)、[所选重放文档 #104](../../island/retired-repository-references_cn.md#ref-df85274ba886)、精确所选提交的 CI、封存的跨归属方 Q01 重放，以及下文的安装前缀托管进程收据。安装进程的 E5 证据边界已通过独立评审。跨归属方 Q03 关闭仍须与 EOS 源码检查门进行精确提交对账；Q04 发布资格验证与标签另行处理。

## 所选运行的前提

固定已合并的 IsOS、EOS、MuJoCo `island/mujoco-sim@0.1.1`、EGIA 精确提交、包收据、依赖锁摘要、已保存图修订、EOS 投影修订和所选 `egia/stages.json` 修订；记录远程集成分支的实际状态。Q01 归档须包含一次全新 home 的 IsOS → Rust EOS gRPC → MuJoCo → EGIA 运行、所选相机/gate/EGIA 检查身份、相同的运行、图修订、投影修订、stream、gate、generation、帧 ID、JPEG SHA-256 与字节长度，以及 IsOS 所有者验证器结果。一次性重复回放仅报告 `generation_stale`：拒绝加一、接受不增加、已接受身份及阶段、提示、任务和模型状态不变；使用后和 Stop 后私有测试套接字都不存在。还须归档全新 home 的故障、重启、安全停止、旧图和不支持的未来版本路径。冻结设计用例检查器或孤立的 EGIA stub 不能代替此收据。Q01 或 Q02 未完成时 Q03 不能关闭。

## 检查门与故障证据

先运行 `uv sync --extra dev --extra vec`，再运行 `uv run ruff format --check .`、`uv run ruff check .`、`uv run pyright`、`PYTHONPATH=src:libexec uv run lint-imports`、顺序执行的 `uv run pytest -q`、聚焦阶段/Vision/link/重启及事件/恢复测试、两个冻结用例检查器、`./bin/islandctl validate` 和 `git diff --check`。记录工具版本、完整标准输出/错误、退出码和任何首次失败。完整测试前确保另一所选 EGIA 进程未占用固定端口 `127.0.0.1:16060`。所选修订上的受跟踪 CI 作业须运行源码和安装 wheel 检查门，保存作业链接及产物。

从该提交构建 `egia==0.1.0` wheel，在私有前缀内全新隔离的 Python 3.12 环境及全新 IsOS home 中安装，并证明 `egia` 和 `island` 都从该前缀导入而非源码检出。CMake 配置须传入 `-DCMAKE_INSTALL_PREFIX=<prefix> -DISLAND_INSTALL_ETCDIR=<prefix>/etc/island`，然后运行 `cmake --install`；默认 `/etc/island` 在测试前缀外。公共 `egia`、`egia-config`、`islandctl` 包装器由 CMake 拥有，不是 wheel console script。从前缀调用已安装包装器，当前目录和继承的 `PYTHONPATH` 均不得包含源码检出，并明确设置 `EGIA_PYTHON`、`ISLAND_PREFIX`、`ISLAND_MODE=system`、`ISLAND_ETC`、`ISLAND_SHARE`、`ISLAND_CONFIG`、`EGIA_ETC` 为所选安装路径。核对解析后的配置、每个所选共享资源根、已安装包装器及两个模块导入路径均位于前缀内；出现源码检出、主机 `/etc/island` 或意外路径即失败。从安装前缀运行 setup identity/catalog/validate/render、两个能力探针及 `islandctl validate`，比较精确类型/特性 ID 和所选 `stages.json` 摘要。随后运行所选 IsOS 管理的真实进程路径，核对 EOS 元数据、仅所有者可访问的授权和套接字、七阶段健康、一个精确原语绑定、一个受治理操作的终结收据、相同运行与关联 ID。注入子进程故障并在同一运行重启；用持久操作 ID 核对，不能再次 Submit 或重放运动。EOS 发现不完整/混杂、所选修订错误、授权不安全时须阻断写入或 ready；未解决的终态须保持 pending/锁存。核对授权撤销、Vision 断连和 Stop 清除临时 JPEG、无迟到回调、所有者停止顺序、子进程回收和私有 listener 消失。授权重置要求 EGIA 已停止并取得两次全新完整且安全的 EOS 代际。保存 IsOS 所有者验证器与 EGIA 回放，进程退出日志不足以证明通过。公开 EOS 发布兼容性另须针对精确已发布版本做真实进程回放。

Q03 CI 作业将来自 EOS 契约精确提交 `c43e5a32eafd9bf22b2dba2940662dcded60111c` 的仅测试用 EOS 源码布局归档（`share/`、可执行的 `bin/eosd` 及 `adapters/eos-cxx/CMakeLists.txt` 源码标记）校验 SHA-256 后展开为同级目录 `Island-eos/share`。这为契约测试和 CMake 安装资源合并提供源码配置中的 `../Island-eos/share`。归档不进入 EGIA wheel；真实进程资格验证所选 EOS 运行时修订另行记录。

## 固定的初始基准负载

设计审查后在 `tests/benchmarks/` 增加确定性 runner；测量前提交 README，写明精确的夹具生成器、调度、屏障和调用方法。它们是本地只读测量，不使用网络模型或物理执行器。环境空闲且固定时先预热一次，再测量五次；报告基线之前不设吞吐目标。保存完整固定输入或生成器种子、SHA-256、字节长度、逐项单调时钟时间戳、ID、计数器、原始样本及输出摘要。预期身份或计数不一致即失败。

1. **阶段证据投递。** 组合 `tests/egia/test_stages.py` 的七个 `@v0.1` 类型及两条 A04 路径，使用完整的 stub EOS 代际和 `auto_drain=False`。固定有效的 `sample.text: "ready"`、`sample.eos_robot_id: "robot-a"`、从 `r-00000` 到 `r-09999` 的唯一 resource ID、当前观测时间，并记录负载摘要和大小。以单调时钟调度按每秒 1,000 项提供 10,000 条有序 resource 事件；resource 边 `priority: 0` 且不设 `frequency_hz`。基线时 observations 路径空闲。竞争场景中，resource 设 `frequency_hz: 30`；每个相同的 1 毫秒提供时刻，在发布 resource 后独立发布一个 `{"items":[]}` observations 值，ID 从 `o-00000` 到 `o-09999`，速率每秒 1,000 项；该边 `priority: -10` 且不设 `frequency_hz`。这些合成 observations 不宣称是 resource 经真实 perception 变换所得。每到 `t0 + i × 1 ms` 截止时刻等候到期，迟到也不跳过提供项，按上述顺序发布后恰好调用一次 `drain_ready(limit=1)`。最后一次提供后，每 1 毫秒再排空一次，直到**每条边**满足 `delivered + overflow = offered` 且无待传项；超出提供结束后 2 秒即失败。旧数据或错误诊断不可算作成功投递。记录实际单调时钟入口与 handler 时间戳及墙钟耗时；分别报告 resource 入口→perception、observations 入口→context 延迟，并分别列出两条路径的 ID、提供/接受/投递、溢出与缺口。达到屏障后才 Stop，并断言停止后无投递且待传/丢弃项为零。单槽最新值边可合并事件，不能要求全部提供项都投递。为 runner 增加聚焦的计数、所有者故障和屏障超时测试。result、safety、admission 和命令流不得作为 A04 证据调度。
2. **Vision 只读准入。** 使用 `tests/egia/test_vision_evidence.py` 的 stub 所有者 UDS WebSocket 路径、一张记录 SHA-256 和字节长度的固定有效 JPEG、所选绑定/渲染（`max_width_px: 1280`、`jpeg_quality: 80`、`target_fps: 30`）及已证明的 stream。以最高 30 帧/秒提供 1,000 个严格递增 generation；每 100 张有效帧后插入一张最新 generation 的重复帧，预期恰好十次 `generation_stale` 拒绝，已接受状态不前进。测收到帧到检查可用的延迟及接受帧/秒。重复运行基准不使用一次性 Q01 旧代际测试套接字，其生命周期不同。公开证据不保存 JPEG 字节。

每种负载报告提供数、接受数、投递数（Vision 可用接受数作为终止计数）、墙钟秒数、投递/秒或接受/秒吞吐、每轮以单调时钟计算的毫秒中位数与最近秩 p99、五轮中位数与范围、溢出/缺口/拒绝计数及所有错误。受提供速率限制的结果必须注明，不能称为最大容量。记录 CPU 型号/核心数、内存、OS/kernel、Python/uv/EGIA 版本、精确提交、电源/容器/CPU 亲和设置、并行负载、预热时长、时钟来源、运行 wheel 还是源码，以及输入摘要/长度。原始 JSON/CSV 与完整日志保存在忽略跟踪的 `logs/test/<run-id>/egia-bench/`；评审者还需可持久访问的产物引用。

## 证据与关闭

每条命令使用 `set -o pipefail` 和 `tee` 保存到 `logs/test/<run-id>/` 的独立文件，记录退出码并保留失败尝试。归档环境清单、`git rev-parse`/status、锁摘要、wheel 构建/安装记录、源码和安装检查门、Q01 收据、Q02 审计引用、所选图和操作 ID、所有者事件、故障/重启/Stop 日志及原始基准结果。用同一所选运行及关联 ID 串联包/版本 → Apply 修订 → EOS ready/发现 → EGIA ready → 任务/gate/终态结果 → Stop；按事件契约检查脱敏、轮转和收集器丢失。把实际命令和实测结果写入双语首次使用指南，经独立设计与实现审查，再重跑受影响检查门，才能宣布 Q03 完成。

### 选定代理与 Vision 证据（2026-11-03）

封存的选定运行固定 EGIA 源码提交 `2e7abae02d6538ba9e2519d6a42eb9ca184c34ec`，选定清单 SHA-256 为 `116aef2846750a96275645e4dec3021ad716ce6fb80b05c087a2ce377a74b990`；冻结评审主构建和案例构建收据 SHA-256 分别为 `977293e097779f636cb70bde9f3d5657bc9351b0b9f67534ca7beb41a97ef915` 与 `20b2dc444d4f90ca1e4cf4b8a2404893c53cd20de19a090ce02debf0c539ab5d`。EOS 中忽略 Git 的 `logs/test/20261103-q03-selected-final-15/` 归档保留 `Q01_RESTART_SCOPED_PASS` 和已封存的 `Q01_FULL_VERIFIED`、EGIA 原始归属方 JSONL、任务快照及授权摘要。选定代码身份早于本次文档更新。

EGIA 在同次已就绪的 EOS 运行中发现三个设备和四个原语。Vision 归属方接受了一帧 12,464 字节、640×480 的 JPEG，其 SHA-256 为 `f3ff9f047ca4b88a0ad97d79f6097e68f17ca62a6dc160efdb1337a28961ce2b`，与 EOS 和浏览器对门 `eos/interface/gate/front-preview`、代际 29、来源 `eos/hardware/camera/front` 的观察一致。故障／重启后的运行 ID 和授权摘要均更新；旧运行查询和旧授权撤销检查通过。最终受治理的 `robot.stop` 成功，IsOS 确认 Stop。这些观察验证了选定的本地路由和负面案例，尚不代表任意模型权重执行或正式发布。

后续封存的 EOS `logs/test/20261103-q03-selected-final-17/` 归档固定 EGIA `2e7abae02d6538ba9e2519d6a42eb9ca184c34ec`、EOS `219f031bae9dbfc9d9f53f62c910b9e235bb2138`、IsOS `b625749c38f056736a1d6dd38759e385c9e04b9e`、MuJoCo `3bf8a50aa2100b3eb1758ea4795bdcdddb65391e` 和 IsosUI `90579a9e86033f4180d81d93a212e7b8a61c1850`；选定清单 SHA-256 为 `35dd4e5aa4c209bf84935002a4eded756f3eb5a1c147b2fb12bd5d894e46b78a`。验证器报告 `Q01_RESTART_SCOPED_VERIFIED_PASS` 和 `Q01_FULL_VERIFIED`，并包含六个案例清单。这些是选定的主题分支提交，不是 Q04 的合并后发布重放。

针对该 EGIA 精确提交，[push 源码与安装作业](../../island/retired-repository-references_cn.md#ref-075162f7d189)和 [PR 源码与安装作业](../../island/retired-repository-references_cn.md#ref-ef872dabc63c)均通过。[push 安装收据](../../island/retired-repository-references_cn.md#ref-d5d1a1839ee7)和 [PR 安装收据](../../island/retired-repository-references_cn.md#ref-191da7c422b9)是可下载的 CI 评审产物，但受保留期限限制。五轮阶段与 Vision 测量、wheel／前缀检查、命令、摘要和最初的 Ruff 格式失败均保留在[首次使用指南](../develop/build-and-run.md)。所选真实进程重放使用 EGIA 源码检出的 `.venv`，而 wheel／前缀证据只证明已安装 setup。下文补充的安装前缀真实进程收据关闭了这项 E5 证据边界；EOS Q03 对账和 Q04 发布还有各自的检查门。

### 已安装 EGIA 托管进程证据，2026-11-04

[EOS 安装进程证据](https://github.com/Island-Team/Island-eos/blob/573d7b15/docs/eos/maintenance/q03-evidence.md)保存在 EOS 本地 `logs/test/20261104-q03-egia-installed/`，通过 [EOS PR #347](https://github.com/Island-Team/Island-eos/pull/347) 以 `573d7b15` 合入。所选五个所有者提交包括 EGIA `4db8319c4b0dcd22e9e3af37c6c5c03d1d5dfca8`，该提交的[源码与安装 CI 作业](../../island/retired-repository-references_cn.md#ref-26e039c8e5e4)通过。精确输入 SHA-256 为 `3688ba488a91192e5b16b00cba1dae18a8a80e6578e2edad9794616c93b2dbee`；正向和负向证据封印 SHA-256 分别为 `07dad1b546c58f3683798d5fbd225165c8052c2753b6c4f6b15e2d875759367b`、`5e179082da501496e197171bf61a8403b3d0083f0e8295310b08f39583504cd1`。所选 `egia==0.1.0` wheel 的 SHA-256 为 `c438f76de01a174f7d36599006af5f1442dbb1c6a9c144c51cad274d9b3a09d8`。输入收据固定五个提交与源码树、依赖锁、258 个 CMake 安装文件、236 个 wheel 内容文件、所选 MuJoCo 包及测试程序摘要。

全新前缀的 wheel 与 CMake 安装使 `egia` 和 `island` 都从前缀导入。IsOS 生成 `egia_chain.mode=installed`；托管 EGIA 子进程使用前缀 Python 和库路径，当前目录等于全新 IsOS home，`PYTHONPATH` 没有源码检出。两次独立真实运行均达到七阶段 `egia_ready`，各自完成一次受治理的 `base.velocity` 任务；故障与重启隔离了旧授权和旧操作。`robot.stop`、IsOS 确认 Stop 以及子进程回收通过。未修改的全新 home 对照组达到 ready 并停止；安装模块篡改、所选覆盖文件篡改及错误解释器三个独立启动均在子进程创建前返回 HTTP 422 `generated_manifest_mismatch`，运行身份未变且没有子进程 PID。独立评审按外部封印重跑正负只读验证器，未发现 P1/P2。该收据关闭安装托管进程的 E5 证据边界。封存的 [Q04 final-5](https://github.com/Island-Team/Island-eos/blob/573d7b15/docs/island/q04-merged-qualification.md#fifth-attempt-merged-home-final-5-full-archive-verified) 浏览器和迁移完整归档选用较早 EGIA `cca1ce66`，本次安装检查门选用 `4db8319c`；两份收据均不表示已发布版本。
