# 公开发布检查

[English](../../../docs/eos/maintenance/release.md)

部署配置、原生产品技能、工作流模板、现场脚本和运行状态保留在本地，并由 Git 忽略。干净检出应能构建 EOS core；仿真由 IsOS 管理的外部 MuJoCo 软件包提供。

发布前检查已跟踪源码、测试、文档和 Git 历史，确认其中没有现场名称、服务地址、设备标识、标定数据、已保存路线、凭据、采集文件或私有依赖。从干净检出验证构建和测试，并检查文档链接及发布差异。

在新提交中删除文件不会将其从旧提交或标签中抹除。若发布要求彻底清除历史内容，需要单独规划历史改写并替换已发布引用。

## 0.2.0 快照审查与首次使用

公开源码快照 `0.2.0` 已在公开仓库发布。跨仓库已发布构件的首次使用验收已在经审查的 Q04 记录中通过。逐项检查[受跟踪文件收据](publish_cn.md#准备-020-源码候选版)中的路径。在受跟踪源码树及完整公开差异中检查凭据、密钥、私有主机名或局域网端点、机器人及设备身份、标定数据、路线、采集文件、提示词、现场脚本、可运行默认值中的本地主目录路径，以及私有依赖。历史证据报告中的本地路径要单独分类：它们标识过去的测试环境，不应被悄悄改写成新的测试结果。私有的可选 skill 目录是明确的公开快照排除项；应确认公开源码及文档均不依赖该目录，同时保留其私有可调用 ABI。确认被忽略的部署文件、生成配置、已安装包、运行日志及测试归档均不在公开的受跟踪文件收据中。另行审查公开 Git 历史；新提交删除受跟踪文件，不会抹除旧公开提交中的副本。

从带标签的公开检出目录复现发布关卡时，若根文件系统空间较小，在发行候选检出目录外、空间充足的文件系统上设置 `TMPDIR` 和 `CARGO_TARGET_DIR`。在干净的公开检出目录依次运行 `cargo fmt --all -- --check`、`cargo clippy --workspace --all-targets -- -D warnings`、`cargo test --workspace --all-targets -- --test-threads=1`、`cargo build --locked --workspace --bins`、`cmake -S . -B "$BUILD_DIR" -DISLAND_BUILD_TESTS=ON`、`cmake --build "$BUILD_DIR"`、`ctest --test-dir "$BUILD_DIR" --output-on-failure` 和 `python3 scripts/check_doc_links.py --all`。将完整输出及工具版本保存到发布收据，并与选定私有源码关卡核对后再声明发布结果。

首次使用重放按 [EOS 操作指南](../usage/README_cn.md)使用干净的 IsOS 主目录和已审查的 `island/mujoco-sim@0.1.1` 资源。从带标签的公开检出目录构建 EOS，通过 IsOS 安装并验证软件包，选择可用场景与设备，然后执行 `isos config validate <graph-file>` 和 `isos config apply <graph-file>`。在两个终端分别运行 `isos serve` 和 `isos run`；用 `isos status --json` 和 `isos logs all --lines 200` 找到就绪事件或首个失败事件。检查相机或 world 资源，发送一次准入的 primitive 并取得关联结果，注入故障后重启，最后运行 `isos deployment stop` 并确认子进程已回收。另行验证历史图的显式迁移。记录选定源码、图修订、软件包及二进制哈希、命令输出、故障和停止证据。此流程中的环境路径由操作者指定，不假定检出目录或 MuJoCo 库路径。

可选的选定 P01/P03/U01 重放脚本除归属方、资源、发布描述及输出路径外，还需要 `--mujoco-library-root <已安装-MuJoCo-库目录>`。历史 Q04 final-5 报告中的命令属于其锁定的旧版脚本，保持原记录不变。

2026-11-04 的私有快照准备通过了 `cargo fmt --all -- --check`、带 `--all-targets -- -D warnings` 的工作区 Clippy、重跑后的串行工作区测试、锁定的工作区二进制构建、公开清单及套接字路径 Python 集中测试、Python 编译、CMake/CTest core 检查、可选 C++ 配置及私有 skill 目标构建，以及 Markdown 链接检查。完整记录保存在 Git 忽略的 `logs/test/20261104-q04-eos-public-snapshot-prep/`。首次串行 Cargo 运行期间，两项对时间敏感的 Dispatcher 停止测试在并发负载下超时；两项单独重跑及完整串行工作区重跑均通过。这些属于私有准备关卡，不能代替独立的干净公开快照检查或公开标签首次使用重放。

## 公开标签状态（2026-11-04）

EOS 公开标签 [`0.2.0`](https://github.com/Island-Team/Aether-EOS-R/tree/0.2.0) 的标签对象是 `e23c9f6dbed46c3c286835ca2d1c628e4c16972d`，对应公开源码提交 `a4b66d3dff75d0d4b738a265b160bcd75bb7f34a`。[Q04 发布后记录](../../island/q04-merged-qualification_cn.md#发布后五标签验收2026-11-04已完成)中的五方冻结预检同时固定 EGIA、IsOS、IsosUI、MuJoCo 标签和发布构件哈希。这是发布与输入身份记录，并非最终集成结论。首次五标签运行 `20261104-public-five-release-6` 仅保留限定范围的源码标签证据；下述另行封存的 run-7 已发布构件重放完成发布边界。

首次五标签运行的 Restart 与迁移验证器已通过，但实际提供的 UI 在预检后重新构建，与已发布 UI 构件不同；IsOS 与 EGIA 的实时进程也来自源码构建。这次限定范围的源码标签运行不能验收已发布构件；下述 run-7 构件级重放及独立封存满足了该要求。

另行进行的 [run-7 已发布构件重放](../../island/q04-merged-qualification_cn.md#第七次尝试公开归属方构件2026-11-04已批准)使用发布版 IsOS 二进制与 UI 归档、已安装 EGIA wheel 前缀、EOS 标签构建以及固定的 MuJoCo 构件。主路径、Restart、迁移及确认的 Stop 通过。本地构件验证器在重新封存前通过；独立评审验证了最终 314 成员的封存。UI Vision 因未选来源而保持 `Stopped`；归属方专用套接字相机采集另行通过。所选 EGIA 产品配置为所选公开检出目录改写一个 EOS share 条目；修正后收据的 SHA-256 为 `fdec41133a580fef4ad28d5c4ac6cd5ca1440287307820634e62c09ff8d9ed35`，最终 314 成员封存的 SHA-256 为 `cd0cf013653407c5f68b3c96a4df1091bb4008b79aee4cbdf52338e94469f228`。独立评审接受该来源边界；Final-5 与 run-7 的组合范围满足 Q04/M5 证据关卡，不声称 UI Vision 帧已渲染或公网 HTTPS。本 PR 将经验收的 `refactor(v2)/runtime-main` Git tree 最终同步到远程私有 `master`。
