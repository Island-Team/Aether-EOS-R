# Q03 EOS 所有者证据

[English](../../../docs/eos/maintenance/q03-evidence.md) · [跨仓库关卡](../../island/q03-qualification_cn.md)

**状态：**EOS 主题提交 `f9a320242f7db91c0d2f7b576cac259625ea4df9` 的源码与基准关卡已通过；final-17 选定安装后跨仓库重放及准确源码关卡在 EOS 主题提交 `219f031bae9dbfc9d9f53f62c910b9e235bb2138` 上通过。下文基准属于初始实测基线，不构成性能目标。Q04 发布工作仍待完成。

## 源码与安装检查

在 Q03 EOS 主题工作树中，`/home/operator/.cargo/bin/cargo fmt --all -- --check`、带 `--all-targets -- -D warnings` 的工作区 Clippy、带 `--test-threads=1` 的串行工作区测试、release 与 debug 工作区构建，以及 `python3 scripts/check_doc_links.py --all` 均通过。首次并行工作区测试在依赖编译期间出现三项对时间敏感的 Dispatcher 失败；独立 Dispatcher 测试通过 71/71，完整串行重跑也通过。两份记录都保存在 `logs/test/20261103-q03-eos-source/`；链接检查覆盖 346 个 Markdown 文件且无断链。`cmake -S . -B /tmp/eos-q03-default -DCMAKE_INSTALL_PREFIX=/tmp/eos-q03-default-prefix -DISLAND_INSTALL_ETCDIR=/tmp/eos-q03-default-prefix/etc/island -DISLAND_BUILD_TESTS=ON`、构建、安装及 CTest 在 `logs/test/20261103-q03-eos-install/` 通过 2/2。首次 CMake 配置遗漏 `ISLAND_BUILD_TESTS=ON`，因此 CTest 未找到测试；保留原始记录，并在修正后通过。

## Dispatcher 基线

运行 `/home/operator/.cargo/bin/cargo run --locked --release -p aether-dispatcher --example bench -- --repetitions 5 --output logs/test/20261103-q03-eos-bench-review-fix/samples.json`。五次串行重复、不预热也不显式设置超时；每次包括内嵌旧版 `1.0.0` 安全读取 profile（1,000 次）、同步完成的内存模拟底盘（5,000 次调度）、带同步 journal 的持久模拟底盘（100 次调度）和 640×480 RGB8/depth16 模拟相机（1,000 次捕获）。模拟执行返回终态完成，不构造不确定移动的重试路由。每次调用用 `std::time::Instant` 计时；吞吐量以成功数除以测得的工作负载墙钟时长。偶数样本的中位数取中间两数平均；p99 使用最近秩。下表合并五次重复并累加相应墙钟区间。

| 工作负载 | 成功／尝试 | 拒绝／错误／超时 | 吞吐（次／秒） | 中位数 µs | p99 µs |
| --- | ---: | ---: | ---: | ---: | ---: |
| 旧版 profile 安全读取 | 5,000/5,000 | 0/0/0 | 6,341.16 | 155.40 | 220.42 |
| 内存调度 | 25,000/25,000 | 0/0/0 | 384,678.05 | 2.24 | 4.13 |
| 持久调度 | 500/500 | 0/0/0 | 126.94 | 7,675.75 | 10,772.92 |
| 模拟相机 | 5,000/5,000 | 0/0/0 | 14,156.82 | 60.13 | 319.57 |

逐次原始样本、每次汇总、完整命令输出、环境与源码哈希保存在忽略 Git 的 `logs/test/20261103-q03-eos-bench-review-fix/`；`samples.json` SHA-256 为 `44b809e533b558fe75913b6f8aff32ad7177408a1df4947a29373d0b08dcb4e0`。之前的候选及审查要求的超时统计修复保存在 `20261103-q03-eos-bench/`。独立审查最初发现慢速拒绝／错误被归类为超时的问题；修复后保留实际结果并另记超时标志。复审重新计算全部 35,500 个样本并批准基准。主机：Intel Core i5-12490F、12 个逻辑 CPU、15 GiB 内存、Linux 7.0.0-38-generic x86_64、rustc 1.98.1；无头模拟，不使用 GPU。旧 profile 安全读取吞吐量不是新图准入吞吐量，模拟结果也不代表真实 MuJoCo 或浏览器延迟。

## 选定跨仓库重演

EOS 主题提交 `219f031bae9dbfc9d9f53f62c910b9e235bb2138` 与四个同级仓库的主题提交通过 final-17 安装后六案例矩阵及完整离线验证。准确提交、外部收据摘要、保留的失败尝试和限制见[选定矩阵证据](q03-selected-matrix-evidence_cn.md#final-17-选定主题提交结果2026-11-03)。上文 `f9a3202` 的基准与源码关卡本身不能用于后续重放提交的源码验收或性能结论。[Q04 final-5 记录](../../island/q04-merged-qualification_cn.md#第五次尝试merged-home-final-5完整归档已验证)单独验收选定合并后集成提交；发布评审与标签仍待完成。

后续 `219f031` 源码另行通过准确提交的关卡，不沿用 `f9a3202` 的基准结果。干净的分离检出源码树为 `d74b369daa6784526f3ca6c061d218369346e78e`。以 `TMPDIR=/home/operator/q3tmp` 且构建、安装输出均在 `/home` 运行时，`cargo fmt --all -- --check`、`cargo clippy --workspace --all-targets -- -D warnings`、串行 `cargo test --workspace --all-targets -- --test-threads=1`（378 项通过、一项忽略）、`cargo build --release --workspace`、350 个文档文件的断链检查（零断链）、CMake 配置／构建／安装及 CTest（2/2）均通过。精确命令、环境、可执行文件摘要及各日志 SHA-256 记录在忽略 Git 的 `logs/test/20261104-q03-selected-eos-gates/results.json`（SHA-256 为 `68b0483c9585607e620ad33ac4a1f00ec01592e8f8169bb7b7f51939ebe37d53`），完整输出保存在同目录。独立审查使用该准确检出及外部归档封印 `f86973093cc8792aef9cd8773eb222a2aff3142b0f99204389c2cc008ac6cc8f` 只读验证选定 final-17 归档，结果为 `Q01_FULL_VERIFIED`。上述源码关卡不改变较早基准的源码归属，也不新增性能结论。

## 补充验收：安装后的 EGIA 托管进程（2026-11-04）

Q04 final-5 归档覆盖完整的干净主目录、软件包、浏览器和迁移矩阵。本次补充 Q03 重演验证遗漏的边界：**由 IsOS 实际托管、通过 wheel 与 CMake 前缀安装的 EGIA 进程**。选定提交为 IsOS `76a1d664`、EOS `0bd28b4d`、EGIA `4db8319c`（打包修复 PR #107）、IsosUI `f0d29848` 和 MuJoCo `04b65882`。完整提交、源码树、锁文件、258 个 CMake 安装文件、236 个 wheel 载荷文件及 MuJoCo 发行摘要保存在外部输入收据中。wheel SHA-256 为 `c438f76de01a174f7d36599006af5f1442dbb1c6a9c144c51cad274d9b3a09d8`。构建、临时与测试目录均在 `/home` 下，设置 `TMPDIR=/home/operator/q03t`。

从本主题工作树，以准确的 EGIA `4db8319c` 构建 wheel 与 CMake 前缀 `/home/operator/q03t/20261104-egia-installed-final/prefix`，再按 `logs/test/20261104-q03-egia-installed/live/command-argv.json` 中的完整参数运行 `tests/m4_affected_replay.py`，指定 `--egia-install-prefix`、`--egia-root /home/operator/projects/q03-egia-selected`、`--mission --q01-restart` 和输出目录 `/home/operator/q03t/20261104-egia-installed-final-4/live`。完整参数列表见 `command-argv.json`；标准输出与错误输出见 `logs/test/20261104-q03-egia-installed/logs/live.log`。 参数列表的 SHA-256 为 `8e682e7e203a0844c971f100da493c4664f357f1bb3a8f445eb622d041072fdf`。`ISOS_PHASE_TIMING=1` 仅记录阶段耗时。IsOS 生成的 `egia_chain.mode=installed`；真实子进程使用前缀中的 Python 与 `lib/island`，`PYTHONPATH` 不含源码，`/proc` 工作目录正是新的 IsOS 主目录。IsOS 验证选定七阶段图，并在两次运行中记录 `egia_ready`；两次均执行受治理的 `base.velocity` 任务，使用不同的运行 ID 和授权文件。注入无效模型回复后，在配置的次数上限停止，未产生新的 EOS journal 记录。重启后拒绝旧授权和旧操作；`robot.stop`、确认的 Stop 和 PID 回收均通过。

正例外部输入收据 SHA-256 为 `3688ba488a91192e5b16b00cba1dae18a8a80e6578e2edad9794616c93b2dbee`，归档封印 SHA-256 为 `07dad1b546c58f3683798d5fbd225165c8052c2753b6c4f6b15e2d875759367b`。执行 `python3 tests/verify_q03_egia_installed.py logs/test/20261104-q03-egia-installed/live logs/test/20261104-q03-egia-installed/external/inputs.json --expect-inputs-sha256 3688ba488a91192e5b16b00cba1dae18a8a80e6578e2edad9794616c93b2dbee --expect-evidence-sha256 07dad1b546c58f3683798d5fbd225165c8052c2753b6c4f6b15e2d875759367b` 可只读复核复制后的归档，结果通过。

另用从未启动过的 IsOS 主目录分别完成 Validate/Apply，只将原图中恰好十个运行时路径重定位到新主目录。未修改的对照达到四个进程就绪，确认 Stop 并回收。篡改安装模块、修改选定的 `egia/stages.json`、更换解释器三项均在真实 IsOS Start 中返回 HTTP 422、`generated_manifest_mismatch`、阶段 `snapshot`、空路径；运行身份未变，也没有新子进程。选定 IsOS 的 `manager/mod.rs` 先执行 `verify_selected_before_reader`，后执行 `start_prepared_processes`，因此这些失败发生在创建进程之前。反例完整命令和 `TMPDIR` 见 `logs/test/20261104-q03-egia-installed/external/negative-command-argv.json`（SHA-256 `87e4c63619076de12a655707bedac6e1798e55e2287930f4d6ad70a973d11467`）。反例归档封印 SHA-256 为 `5e179082da501496e197171bf61a8403b3d0083f0e8295310b08f39583504cd1`；`tests/verify_q03_egia_installed_negatives.py` 用三个外部 `--expect-*sha256` 值只读验证 `logs/test/20261104-q03-egia-installed/negative-4`。此前失败尝试保存在 `/home/operator/q03t/`，记录了工厂默认路径、可写目录、驱动资源、源码工作目录和重新打开旧主目录时的恢复状态问题。此补充关卡不替代 Q04 完整浏览器及迁移归档，也不发布标签。 本主题分支的 `cargo fmt --all` 与 `cargo fmt --all -- --check`、含 `--all-targets -- -D warnings` 的工作区 Clippy、串行工作区测试、锁定版本的 debug/release 工作区二进制构建、Python 编译、两项只读证据验证及 `python3 scripts/check_doc_links.py --all` 均通过。Cargo 使用 `CARGO_TARGET_DIR=/home/operator/projects/Island-eos/target` 和 `TMPDIR=/home/operator/q03t`；完整关卡输出在 `logs/test/20261104-q03-egia-installed/gates/`（检查 354 个 Markdown 文件，零断链）。
