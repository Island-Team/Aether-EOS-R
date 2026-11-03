# 公开发布检查

[English](../../../docs/eos/maintenance/release.md)

部署配置、原生产品技能、工作流模板、现场脚本和运行状态保留在本地，并由 Git 忽略。干净检出应能构建 EOS core；仿真由 IsOS 管理的外部 MuJoCo 软件包提供。

发布前检查已跟踪源码、测试、文档和 Git 历史，确认其中没有现场名称、服务地址、设备标识、标定数据、已保存路线、凭据、采集文件或私有依赖。从干净检出验证构建和测试，并检查文档链接及发布差异。

在新提交中删除文件不会将其从旧提交或标签中抹除。若发布要求彻底清除历史内容，需要单独规划历史改写并替换已发布引用。

## 0.2.0 候选版审查与首次使用

公开源码快照 `0.2.0` 在独立公开 PR、干净检出关卡和跨仓库首次使用验收通过前仍是**候选版**。逐项检查[受跟踪文件收据](publish_cn.md#准备-020-源码候选版)中的路径。在受跟踪源码树及完整公开差异中检查凭据、密钥、私有主机名或局域网端点、机器人及设备身份、标定数据、路线、采集文件、提示词、现场脚本、可运行默认值中的本地主目录路径，以及私有依赖。历史证据报告中的本地路径要单独分类：它们标识过去的测试环境，不应被悄悄改写成新的测试结果。私有的可选 skill 目录是明确的公开快照排除项；应确认公开源码及文档均不依赖该目录，同时保留其私有可调用 ABI。确认被忽略的部署文件、生成配置、已安装包、运行日志及测试归档均不在公开的受跟踪文件收据中。另行审查公开 Git 历史；新提交删除受跟踪文件，不会抹除旧公开提交中的副本。

若根文件系统空间较小，在发行候选检出目录外、空间充足的文件系统上设置 `TMPDIR` 和 `CARGO_TARGET_DIR`。在干净的公开检出目录依次运行 `cargo fmt --all -- --check`、`cargo clippy --workspace --all-targets -- -D warnings`、`cargo test --workspace --all-targets -- --test-threads=1`、`cargo build --locked --workspace --bins`、`cmake -S . -B "$BUILD_DIR" -DISLAND_BUILD_TESTS=ON`、`cmake --build "$BUILD_DIR"`、`ctest --test-dir "$BUILD_DIR" --output-on-failure` 和 `python3 scripts/check_doc_links.py --all`。将完整输出及工具版本保存到发布收据，并与选定私有源码关卡核对后再声明发布结果。

首次使用重放按 [EOS 操作指南](../usage/README_cn.md)使用干净的 IsOS 主目录和已审查的 `island/mujoco-sim@0.1.1` 资源。从候选检出目录构建 EOS，通过 IsOS 安装并验证软件包，选择可用场景与设备，然后执行 `isos config validate <graph-file>` 和 `isos config apply <graph-file>`。在两个终端分别运行 `isos serve` 和 `isos run`；用 `isos status --json` 和 `isos logs all --lines 200` 找到就绪事件或首个失败事件。检查相机或 world 资源，发送一次准入的 primitive 并取得关联结果，注入故障后重启，最后运行 `isos deployment stop` 并确认子进程已回收。另行验证历史图的显式迁移。记录选定源码、图修订、软件包及二进制哈希、命令输出、故障和停止证据。此流程中的环境路径由操作者指定，不假定检出目录或 MuJoCo 库路径。

可选的选定 P01/P03/U01 重放脚本除归属方、资源、发布描述及输出路径外，还需要 `--mujoco-library-root <已安装-MuJoCo-库目录>`。历史 Q04 final-5 报告中的命令属于其锁定的旧版脚本，保持原记录不变。

2026-11-04 的私有快照准备通过了 `cargo fmt --all -- --check`、带 `--all-targets -- -D warnings` 的工作区 Clippy、重跑后的串行工作区测试、锁定的工作区二进制构建、公开清单及套接字路径 Python 集中测试、Python 编译、CMake/CTest core 检查、可选 C++ 配置及私有 skill 目标构建，以及 Markdown 链接检查。完整记录保存在 Git 忽略的 `logs/test/20261104-q04-eos-public-snapshot-prep/`。首次串行 Cargo 运行期间，两项对时间敏感的 Dispatcher 停止测试在并发负载下超时；两项单独重跑及完整串行工作区重跑均通过。这些属于私有准备关卡；干净公开快照的关卡仍须执行。
