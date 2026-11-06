# M2 全新主目录验收

[English](../../docs/island/m2-qualification.md) · [里程碑](implementation-milestones_cn.md) · [详细 TODO](implementation-todo_cn.md) · [固定用例](../../tests/fixtures/m2_qualification/README_cn.md)

**锁定输入的 M2 重放已通过 28/28 条用例。** 本页记录跨仓库 M2 首次使用关卡。IsOS 拥有图原字节、包收据、生成配置、进程状态和已接收日志的顺序；MuJoCo 包拥有场景与设备元数据；EOS 和 EGIA 公告自身读取能力并发送有界事件；IsosUI 展示 IsOS 状态。字段和诊断以各所有者的 `interfaces/` 页面为准。28 条固定用例分六批通过；M2-28 是隔离的 IsOS 读取器谓词测试，因为公开的按锁定组装目录路径无法生成其合成不匹配。

## 固定输入与隔离主目录

重放在使用程序前核对精确 `git rev-parse HEAD`。已合并基线为 EOS `refactor(v2)/runtime-main` 的 `f22d6e7c65ab92b3ed140c5f49e1f12f33232ffc`，以及 `master` 上的 IsOS `ace8f4c60b8a74b3a998fe2ae78da4c38afa2020`、EGIA `a03981fadf88d87e13f33a3ada577c6a1a8dd0df`、MuJoCo `d3381130ff43784ee6c7f652888ae1e5aa625fb9`、IsosUI `bea957ce3a5b023f15727978a832186db135a159`。私有 `logs/test/<run-id>/` 记录这些 SHA、主机系统/架构、MuJoCo 库路径和摘要、所选程序摘要、命令及响应。不能以分支名或产品版本代替所选字节。每条用例使用临时空 `ISOS_HOME`、独立回环端口、匹配的 MuJoCo 3.16.0 库，以及新建的 `island/mujoco-sim@0.1.1` 归档或链接候选。共享事件仅存放脱敏值。

先完成五个所有者仓库要求的格式、lint、测试与构建检查。使用 MuJoCo 包的 `tools/build-backend-bundle.py` 准备包；v4 发布清单、归档、`backend-v2.json`、场景 XML 和引用资源必须齐全。在两个独立空主目录中分别执行 `isos pkg install --manifest <release> --asset <archive> --allow-unapproved` 与 `isos pkg link island/mujoco-sim 0.1.1 <bundle> --manifest <release>`。核对 schema-2 收据中的精确锁定、来源类型、发布和载荷摘要；安装或链接不得改变保存图字节或活动状态。另用一个既未显式指定清单、也找不到 `release.json` 的包目录执行链接：链接成功并生成 schema-1 收据，列表状态为 `unverified_legacy_link`，但 Apply 和 Run 在精确进程 `/package` 指针处以 `package_invalid_manifest` 拒绝。归档摘要错误或符号链接逃逸则在直接操作时拒绝，不发布收据。不要把主目录路径、收据全文或密钥放入公开事件。

## 首次使用重放顺序

1. 在隔离主目录启动 `isos serve`。任何运行获准前，读取 `process-status` 和 P07 `view=current`，应看到管理器会话范围和等待中的 `first_prelaunch_failure` 锚点。触发一次坏包链接或 Apply，证明首条类型化包/Apply 故障固定在过滤后的 `lines` 之外；仅 Validate 故障不得成为该锚点。
2. 链接或安装有效精确包。针对 `pick-place` 和 `navigation` 预览 `sim-hardware`。将各草稿 POST 到 `/api/isos/v1/node-options`；选择可用的稳定场景、相机、底盘和机械臂 ID，并匹配 `simulator_node`。切换场景后重新查询。不可用的已保存 ID 保持可见但 Apply 失败；路径形式的非法 ID 在准确设置指针处失败。预览与选项读取不改变保存字节。
3. Validate 并按修订 Apply `0.1.0` 图。再次检查收据、所选场景 XML/配置/摘要、库、私有生成的 schema-2 快照、所选 EOS 和可选 EGIA 程序以及保存修订。对同一修订重复 Apply，验证幂等。通过 P05 检查实际所选 setup/runtime 身份与必需特性。缺少图读取或运行特性时，在 `/version` 或所需进程的 `/node_type` 返回 `reader_feature_unsupported`；已注册精确类型不在所选读取器集合中时，在该节点的 `/node_type` 返回 `reader_type_unsupported`。未注册类型先由 G01 返回 `unsupported_node_type`；被改动的 sidecar 先由 P01 在 `/package` 返回 `package_invalid_sidecar`；公开路径中的未安装锁定先由 P01 在进程 `/package` 返回 `package_missing_file`，读取器预检尚未开始。IsOS 另以隔离的 `contributed_type_needs_exact_catalog_pin` 谓词测试公开目录组装无法产生的不匹配：对锁定 0.1.1 的图注入 0.1.0 目录贡献，会在 `/package` 返回 `reader_package_pin_mismatch`。所有拒绝都发生在启动或停止活动子进程之前。程序、默认配置或解释器/模块清单发生变化时，需要显式重新 Apply。
4. 运行仅启用 EOS 的图。比较 `process-status` 的保存/活动修订、run ID、子进程 PID 和所有者就绪事件。运行获准后，部署 `view=current` 范围从未过滤的首条就绪或故障锚点开始，然后才应用详情过滤。PID、监听套接字、启动行或单条日志都不能单独证明就绪。启用 EGIA 的图是另一条反向用例：所选 setup 与 runtime 的精确身份公告读取特性，但公开 Validate 和 Apply 因 R02 前缺少完整 EOS gRPC Gateway 端点，在 `/cores/egia` 返回 `generated_owner_rejected`。不宣称 EGIA 已 Run 或就绪。
5. 在锚点之后用来源、节点、关联 ID 过滤读取 P07 页面。以有界事件触发普通轮换；仍保留的游标无 gap 继续。让游标超出保留范围，期待 `log_cursor_expired` 和同一运行内重定位；合法 Restart 产生新运行后，期待 `log_run_changed`、新范围与新锚点。Stop、失败、PID 丢失和管理器重启仍保留最后获准运行身份。过滤后空页且游标前进、无诊断表示没有匹配项。注入采集器中断与恢复，期待 `log_collector_gap` 或 `log_anchor_unavailable`，计数已知时给出数字，否则为 null；不得虚报零丢失。无法证明运行身份时返回 HTTP 503 `log_scope_unavailable`，不得退回管理器范围。
6. 在一次性链接内容中改变资源或程序字节，再尝试新的 Apply、Run 和活动 Restart。P01/P02 类型化拒绝应发生在保存字节发布、停止或更换子进程之前；比较先前 PID 和活动修订。对未来图版本、不可用场景/设备、分别发生的读取器特性与精确类型不匹配、未注册或未验证的贡献、错误精确锁定及生成快照篡改重复反向检查。重启失败时按管理器会话关联排查，不能把它改标为仍保留的部署运行。通过显式重新链接或 Apply 恢复已验证输入，随后 Stop 并确认所有者状态与终态事件；执行器先停止再回收模拟器属于后续 M3 证明。

M2-21 分两层取证。公开重放在 Apply 后把 `cursor-state.json` 替换为目录，使 Run 在没有子进程启动的情况下于 `/run_id` 返回 HTTP 500 `log_scope_publish_failed`。该操作阻止发布前读取状态，并未注入父目录 fsync 故障。IsOS 所有者单元测试分别覆盖保存前故障和模拟的重命名后不确定结果，证明幸存的新运行保留身份且锚点报告不可用。真正的父目录 fsync 系统调用失败不属于此次重放证据。

[机器可检查用例](../../tests/fixtures/m2_qualification/README_cn.md)列出每条正向和反向边界。运行器把完整命令输出、JSON 响应和状态快照保存在被 Git 忽略的运行目录，复制给人的摘要须脱敏，并同时比较保存**字节**和修订摘要。即使功能操作成功，找不到规定的首事件也意味着 M2 未通过。

## 可重现证据

在此 EOS 工作树中运行 `MUJOCO_LIBRARY_ROOT=/home/operator/mujoco-3.16.0-linux-x86_64/mujoco-3.16.0 bash tests/run_m2_qualification.sh`。脚本读取夹具中的五个精确提交，分六批运行；若用例失败、缺失、重复，或程序及库摘要不一致，就拒绝汇总。它将顶层输出保存为 `logs/test/<run-id>/replay.log`，并保存各用例命令和 HTTP 记录、环境摘要及最终 `gate/aggregate.json`。可用 `M2_LOG_ID` 指定唯一运行目录。先构建五个所有者仓库；本脚本验证所选程序并执行 M2 边界，不替代各仓库的格式、lint、测试和构建关卡。

修正后夹具的重放命令为 `M2_LOG_ID=20261101-m2-review-final-v2 MUJOCO_LIBRARY_ROOT=/home/operator/mujoco-3.16.0-linux-x86_64/mujoco-3.16.0 bash tests/run_m2_qualification.sh`。`logs/test/20261101-m2-review-final-v2/` 下的 `a`、`b1`、`b2`、`c`、`d`、`e` 六批全部通过，共 28 条；`gate/aggregate.json` 核对完整覆盖和六份 `batch-attestation.json`。每份证明绑定精确的夹具与运行器 SHA-256、所有者提交、所选 EOS/IsOS/EGIA 程序、EGIA setup/解释器/模块目录、IsosUI 构建、MuJoCo 库、逐用例记录摘要，以及该批归档、发布清单、描述符和载荷摘要。构建时间戳可能使包归档不同，因此汇总器核对每批的精确输入，而不声称六批使用相同归档摘要。`aggregate-tamper-negative.log` 记录了修改 M2-03 事件记录、包载荷清单和过期运行器指纹均被拒绝。先前六批已由此次证据取代。主机为 Linux x86_64、Python 3.12、所选 EGIA `.venv/bin/python`、MuJoCo 3.16.0，并使用独立回环端口；精确系统字符串和所选字节摘要见各批 `environment.json`。冻结的 `cases.json.status` 值 `replay_pending_reattest` 记录重放前夹具状态；修改该值会使六批证明失效。当前结论以已验证的汇总结果和本验收记录为准。

最终本地检查均通过：`python3 tests/fixtures/m2_qualification/check_cases.py`、`python3 scripts/check_doc_links.py --all`、`python3 -m py_compile tests/m2_cross_repo_replay.py`、`bash -n tests/run_m2_qualification.sh`、`/home/operator/.cargo/bin/cargo fmt --all -- --check` 和 `git diff --check`。完整输出位于同一被忽略的 v2 运行目录中的 `*-final-check.log`。

此次跨仓库重放沿用经评审切片的所有者检查日志。它们是对应仓库中被 Git 忽略的本地证据；本次验收未重新运行每个所有者的完整套件。EOS P06 在 `Island-eos/logs/test/20261101-p06-jsonl-events-impl/` 留有 `cargo fmt --all -- --check`、`cargo clippy --workspace --all-targets -- -D warnings`、工作区测试和构建记录。IsOS P07 在 `isos/logs/test/20261101-p07-log-cursors-review-fixes/` 执行 `cargo fmt --all -- --check`、`cargo clippy --all-targets -- -D warnings`、`cargo test --no-fail-fast` 和 `cargo build --all-targets`；`commands.txt` 记录精确环境覆盖。EGIA P06 在 `Island-egia/logs/test/20261101-p06-egia-implementation/` 执行 `uv run ruff check .`、`uv run pytest`（826 条通过）、`uv run pyright`（零错误）、`PYTHONPATH=src:libexec uv run lint-imports`（六条契约保留）和 `./bin/islandctl validate`，详见 `commands.txt`。IsosUI P07 在 `IsosUI/logs/test/20261101-p07-isosui-review-fixes/` 使用 Node 22.23.2/npm 10.9.9 执行 `npm ci`、`npm test`、`npm run typecheck` 和 `npm run build`；`commands.txt` 与输出证明 83 条测试和两个构建包通过。MuJoCo 的 `ctest --test-dir build --output-on-failure` 证据在 `isos-mujoco-sim/logs/test/20261031-doc-alignment/ctest.log`（10 条通过），构建证据在 `isos-mujoco-sim/logs/test/2026-10-30-v2-live/build.log`。这些日志记录经评审切片的已有所有者检查，上述六批则记录所选合并版本的 M2 边界。

此前单进程执行在 M2-16 之后以退出码 143 中断，未计入通过证据。开发期间 EGIA 用例曾从不含 pytest 的虚拟环境调用 pytest；最终 M2-13 直接执行所选 `egia --capabilities` 和 `egia-config setup --json`，并已通过。M2-17 使用真实有界 LogStore 轮换；M2-20 原子替换活动链接程序并确认 Restart 拒绝时活动修订、PID 和保存字节不变；M2-21 结合状态路径受阻产生的公开 HTTP 500 与 IsOS 保存前故障、模拟重命名后不确定结果的所有者单元测试，并未宣称真正的父目录 fsync 故障；M2-28 执行隔离的 `contributed_type_needs_exact_catalog_pin` 谓词。各用例记录保留了这些区别。

P07 的 IsOS [PR #51](retired-repository-references_cn.md#ref-eed25951a226) 和 IsosUI [PR #36](retired-repository-references_cn.md#ref-5917ee61bfce) 均在此次重放前合并。修正后夹具的六批重放在锁定输入上通过 M2 包验证、选择、生成配置、所选读取器准入、仅 EOS 启动、结构化事件和游标恢复；汇总及篡改反向检查均通过。R02 将证明 EGIA 对完整 EOS gRPC Gateway 的发现；R03/R05 将证明受控动作及执行器先停止、再终止并回收模拟器。M5 将重放包含基准和发布证据的操作路径。此次 M2 结果不宣称这些后续边界通过。
