# Q04 合并提交验收证据

[English](../../docs/island/q04-merged-qualification.md) · [Q03 验收](q03-qualification_cn.md) · [跨仓库 TODO](implementation-todo_cn.md)

**状态：第五次合并提交归档通过完整离线验证；Q04 发布工作仍开放。** Q03 final-17 归档验收的是五个选定主题提交。Q04 针对实际已合并的集成分支重放安装后操作路径，审查完整归档与外部收据，再完成各产品独立的发布关卡。首次失败尝试编号为 `20261103-q04-merged-final-1`。临时检出目录删除前，其文件已逐字节复制到被 Git 忽略的 `/home/ashex/projects/Island-eos/logs/test/20261103-q04-merged-final-1/` 归档；文件内记录的源码路径仍指向 `/tmp/q04-merged-eos/`。

## 冻结的合并输入

这次失败尝试的评审人冻结选定文件为 `/tmp/q04-merged-review-20261103-q04-merged-final-1/selection.json`，SHA-256 为 `0575c8487f10556605a87844273fa2783e65d6c076374e958d74bdb544b9c1e6`。此次 Q04 尝试沿用验证器现有的 `selected-q03` profile；此名称不意味着输入仍是主题提交。文件中的归属方分支及准确提交如下：

| 归属方 | 集成分支 | 选定提交 |
| --- | --- | --- |
| EOS | `refs/heads/refactor(v2)/runtime-main` | `3fb3373f4db436c3db861200ef02e1121d8f7a94` |
| IsOS | `refs/heads/master` | `4aa76cd1cc82b63829bbcdbf19336f342574ac8b` |
| EGIA | `refs/heads/master` | `5ca24ecf77ec1c4b4c3fc9eb33fcc8f4d2cfcf82` |
| IsosUI | `refs/heads/master` | `f0d29848b57ba34e2557cc978fb294912064cc5c` |
| MuJoCo | `refs/heads/master` | `c9cb804e977acd585e94d9021e7fe7bcc6a3ce32` |

冻结选定文件中的重建 `island/mujoco-sim@0.1.1` 候选包，其 release SHA-256 为 `af08700997448deb4d699694ef4e1faf0b83652f82bc00468ace3731abe5ee49`，归档 SHA-256 为 `746b413b771e869e41929075fc92784cf4aa3fa0c53f2f32ddd2b23cfad95ffc`。源提交是上表 MuJoCo 已合并的 `master` 提交。这些摘要仅标识候选输入字节，不表示软件包已发布。

## 首次尝试结果与下一关卡

`main.log` 记录确认的 `M4_STOP` 和 `Q01_RESTART_SCOPED_PASS`。`camera`、`g06-r03`、`p01-p03-u01` 案例日志通过。`p07-u03.log` 记录了案例归档创建；创建本身不代表案例验证完成。`r06-a03.log` 以 HTTP 422 的 `PUT node-graph` Apply 错误结束：诊断码 `generated_missing_input`、路径 `""`、阶段 `binary`，消息为 `Restore the selected installation and Apply again`。迁移案例及完整离线验证均未运行。准确来源是上述归档中的 `r06-a03.log` 与 `main.log`；原始案例文件留在其 `live/cases/` 树中。

运行期间根文件系统约剩余 35 MiB，归档在临时检出目录删除前复制。该观察不能证明 Apply 诊断的原因。另一项 MuJoCo 发布审计发现元数据/RUNPATH 阻塞；须完成聚焦的发布修复、重新选定合并提交并重放，才能验收 Q04。此尝试的选定文件和软件包摘要仅是历史输入身份，不能当作下次运行的锁定值。

下表仅记录 final-1；final-5 的验收结果见下方单独章节。

| 首次尝试证据 | Final-1 结果 |
| --- | --- |
| 干净 home 实时重放、六组矩阵案例、故障/Restart 与安全 Stop | 首次尝试未完成：主路径通过、三个案例通过、P07/U03 归档已创建、R06/A03 Apply 失败；须重新完整运行。 |
| 本机及模拟托管浏览器观察、命令/结果及 Vision 关联 | 首次尝试有观察文件，但不能验收未完成的矩阵；须重新运行并审查归档。模拟 Origin 不证明公网 HTTPS 部署。 |
| 外部主构建及案例构建收据与 SHA-256 | 首次尝试文件仅作诊断；final-5 的已验收收据单独列于下方。 |
| 归档校验和清单及 `tests/verify_q01.py` 完整离线判定 | Final-1 未运行；final-5 的已验收封存和验证单独列于下方。 |
| 归属方关卡、基准及双语首次使用审查 | Final-1 未建立此证据；见归属方记录及下述 final-5。 |
| EOS 集成分支合入 `master` 以及选定的 0.x 产品标签 | Final-1 未建立此证据；仍是独立的 Q04 发布步骤。 |

只有针对新的准确选定文件、两份外部收据和完整校验和归档的验证器通过，才能验收合并提交重放。MuJoCo 源码/软件包修复已引出下述第二次选定；该次因另一端点错误在启动前失败。适用的公开快照、产品发布和远程标签关卡通过前，Q04 保持未勾选。

## 第二次尝试：merged-home-final-2（启动前失败）

新冻结选定文件的 SHA-256 为 `4da1c95c77dce5fff8757fcb3838ef3f009e252eb19eb75725bf95f3f9b4b47d`，也记录在 `live/q03-selection-sha256.json`。五个准确集成提交分别为 EOS `c8000bbb6f3f82ca027edba0e129c92a9d3d740a`（`refactor(v2)/runtime-main`）、IsOS `4aa76cd1cc82b63829bbcdbf19336f342574ac8b`、EGIA `5ca24ecf77ec1c4b4c3fc9eb33fcc8f4d2cfcf82`、IsosUI `f0d29848b57ba34e2557cc978fb294912064cc5c`、MuJoCo `04b65882cd11b0aff8ce56473164691dbd662196`（另外四个仓库分支为 `master`）。重建 MuJoCo 候选归档 SHA-256 为 `bb2862b01c3ccab8b68570e766a0ad26c8c5783b3c87cf99225eb0edb7a260f4`；release SHA-256 为 `c421039753c24ffc74658d6b1dfe5c9de6a1ed77b64df8fbaee77063da837096`。

忽略 Git 的归档位于 `/home/ashex/projects/Island-eos/logs/test/20261103-q04-merged-home-final-2/`。`live/run.log` 记录 IsOS 的 `vision_endpoint_unavailable: repair private control-grant state before launching`；`main.log` 以 process-status 持续返回 HTTP 503、子进程运行就绪条件始终未满足结束。归属方调查发现检查 Unix 套接字的路径长 111 字节，超过 Linux 路径上限 107 字节。本次未启动选定的子进程路径、未捕获浏览器结果、未运行六组案例、未封存完整归档，也未执行完整离线验证。上述选定文件和软件包摘要仍是这次失败尝试的冻结身份；首次尝试的结果不能转移过来。

IsOS 与 EGIA 协同完成端点归属修复后进行了下述 final-3 选定。该运行仍须记录自己的外部收据、浏览器与 Vision 关联、六组案例、归档封存和独立验证。此前两次尝试均不能验收 Q04。

## 第三次尝试：merged-home-final-3（验收失败）

冻结选定文件 SHA-256 为 `124766815290e1eaeeacdd975816ea7c062e3f190b86cc8e0878f423d4676623`，记录在归档的 `live/q03-selection-sha256.json`。各归属方提交为 EOS `9c0c632ec9ac7d469abef063820620e552c5cbee`、IsOS `76a1d6644ec4e73a0bd47105652d9bbc18a9cae3`、EGIA `cca1ce66dc2481e2473b55325bc453da1b61c44c`、IsosUI `f0d29848b57ba34e2557cc978fb294912064cc5c`、MuJoCo `04b65882cd11b0aff8ce56473164691dbd662196`。MuJoCo 候选归档/release 摘要与上述 final-2 相同。忽略 Git 的归档位于 `/home/ashex/projects/Island-eos/logs/test/20261103-q04-merged-home-final-3/`。

本次通过短端点启动前边界，并产生真实的实时观察。`live/browser-dom-observation.json` 记录可见 IsosUI Control 命令 `ui-b952e23f-a913-4bd9-b2ec-3461e2dcd20f` 为 `Completed`；`live/browser-result.json` 记录对应 HTTP 200 的 bridge 结果。`live/browser-visible-vision.json` 记录 Vision 面板 `Receiving frames`、generation 125、640×480 和 JPEG SHA-256 `8ec10f7a08c0a9b0638c22e05b00302ae975feaeef0743a48e86e8e071be2001`。`main.log` 记录确认的 `M4_STOP`；`live/reap-proof.json` 记录 Stop 后立即 `all_absent: true`，`live/q01-step-order.json` 保留重启与停止顺序。

验收失败是因为冻结的捕获辅助程序在 IsOS 改用短 `run/v` 端点后，仍从 control-grants 构造迁移前的 Vision 套接字路径。捕获重试以 `FileNotFoundError` 结束；`FAILED_HARNESS.txt` 标明过期辅助程序。`main.log` 以 `both hosted editor observations required` 断言退出 1；验证器要求的托管/浏览器原始证据缺失。六组案例矩阵和完整归档封存均未完成。实时 UI 与 Stop 观察只是诊断证据，不能替代缺失的验证输入。

EOS harness [PR #343](https://github.com/Island-Team/Island-eos/pull/343) 已将路径修正合入 `refactor(v2)/runtime-main`，提交为 `f8945d7a`。下述 final-4 使用包含该修正的新选定文件；其剩余矩阵、收据、封存和验证关卡单独跟踪。Q04 保持未勾选。

## 第四次尝试：merged-home-final-4（实时通过，案例验证器失败）

新冻结选定文件 SHA-256 为 `5972e0dd386beb516bbb97f59ccb9b9f195e63b0557e6f0caff80c564a1ab190`，记录在 `live/q03-selection-sha256.json`。该文件锁定 EOS `refactor(v2)/runtime-main` 的 `f8945d7a813973f8ba389c78b543f98286d99f61`，以及 IsOS `76a1d6644ec4e73a0bd47105652d9bbc18a9cae3`、EGIA `cca1ce66dc2481e2473b55325bc453da1b61c44c`、IsosUI `f0d29848b57ba34e2557cc978fb294912064cc5c`、MuJoCo `04b65882cd11b0aff8ce56473164691dbd662196` 各自 `master` 集成分支提交。MuJoCo 候选归档 SHA-256 为 `bb2862b01c3ccab8b68570e766a0ad26c8c5783b3c87cf99225eb0edb7a260f4`；release SHA-256 为 `c421039753c24ffc74658d6b1dfe5c9de6a1ed77b64df8fbaee77063da837096`。忽略 Git 的归档位于 `/home/ashex/projects/Island-eos/logs/test/20261103-q04-merged-home-final-4/`。

`main.log` 记录确认的 `M4_STOP` 与 `Q01_RESTART_SCOPED_PASS`。可见 IsosUI Control 命令 `ui-b23866f8-6f37-40e5-a59b-5fd3e892055b` 经 Sending → InProgress → Completed（`live/browser-dom-observation.json`）；`live/browser-result.json` 记录相同命令 ID、HTTP 200 及 `Completed` 的公开 bridge 后续结果。`live/browser-visible-vision.json` 记录已渲染的 640×480 帧、generation 114、JPEG SHA-256 `2a7c265add6a020a20b9e50509687633c8ce38748301ead5076a9ffdc72bbd98` 以及 `Receiving frames`。这些是本次运行的实时观察，不是完整离线验证结论。

`camera.log`、`g06-r03.log` 与 `p01-p03-u01.log` 以通过标记结束。`p07-u03.log` 记录案例归档创建，但没有案例完成判定。随后 R06/A03 在 EGIA 进程重启 pytest 子进程失败：`live/cases/r06-a03/egia-process-restart/pytest.log` 表明选定的 `/home/ashex/projects/q04-review/egia/.venv/bin/python` 缺少 `pytest`，`r06-a03.log` 以子进程断言失败结束。迁移、六组案例矩阵、外部案例收据、校验和封存以及独立的 `tests/verify_q01.py` 完整判定均未完成。

测试环境已在选定源码之外修复：在 EGIA 虚拟环境安装 `pytest` 与 `pytest-asyncio`，然后从准确 EOS 检出目录复制预期的 `../Island-eos/share` 同级目录，并逐字节比较。`build/environment-recovery.json` 记录命令、依赖版本、复制的 148 个 share 文件及选定文件 SHA-256；`build/a03-recovery-pytest.log` 记录七项通过、一项警告。该单独检查不能追认冻结的 R06/A03 失败案例通过。

独立评审允许仅恢复案例阶段。失败的案例树和五份案例日志原样移至 `/home/ashex/projects/Island-eos/logs/test/q04-home-replay-prep/runs/20261103-q04-merged-home-final-4/external/failed-cases-attempt-1/`；其 `manifest.json` 保存 1,010 个文件的 SHA-256 条目。`build/recovery-preflight.log` 记录重新执行的软件包/源码预检，`build/recovery-selection-verify.log` 验证五个选定远程分支、受跟踪的 UI 构建输入及 37 个冻结 harness 哈希。

新的完整六组案例重跑已到达实时 R06/A03 行为，EGIA 进程重启 pytest 七项通过、一项警告。随后选定 EOS 案例验证器在 `tests/verify_q01_r06_a03.py:269` 失败：它断言 `required_libraries` 必须为空，但冻结且已审查的 MuJoCo release 声明了非空列表。`r06-a03.log` 保存断言回溯，`FAILED_HARNESS.json` 记录 `failed_qualification`、无案例收据、无封存、无完整验证。此前 `pytest` 环境缺口已解决；这是另一个选定 harness 源码错误。即使保留实时与此前案例观察，此结果仍不能验收 final-4。聚焦 EOS 验证器 [PR #344](https://github.com/Island-Team/Island-eos/pull/344) 已合入 `0bd28b4d`；下述 final-5 使用该修正及新冻结选定文件。Q04 保持未勾选；公开发布及 EOS 集成分支合入 `master` 是单独关卡。

## 第五次尝试：merged-home-final-5（完整归档已验证）

冻结选定文件 SHA-256 为 `7f5cefa5b6521d42d28b88c916679f2b668ba93fd61bbcbbfed4bb8fcc722b87`，记录在 `live/q03-selection-sha256.json`。该文件锁定 EOS `refactor(v2)/runtime-main` 的 `0bd28b4d2c3ba90a43b81c02ea4e883ef5043db0`，以及 IsOS `76a1d6644ec4e73a0bd47105652d9bbc18a9cae3`、EGIA `cca1ce66dc2481e2473b55325bc453da1b61c44c`、IsosUI `f0d29848b57ba34e2557cc978fb294912064cc5c`、MuJoCo `04b65882cd11b0aff8ce56473164691dbd662196` 各自 `master` 集成分支提交。MuJoCo 候选归档 SHA-256 为 `bb2862b01c3ccab8b68570e766a0ad26c8c5783b3c87cf99225eb0edb7a260f4`；release SHA-256 为 `c421039753c24ffc74658d6b1dfe5c9de6a1ed77b64df8fbaee77063da837096`。`build/selection-verify.log` 记录五个选定提交及远程分支、103 个受跟踪的 UI 构建输入和 37 个 harness 哈希均已验证。忽略 Git 的归档位于 `/home/ashex/projects/Island-eos/logs/test/20261103-q04-merged-home-final-5/`。

实时主路径及浏览器捕获通过。`main.log` 记录确认的 `M4_STOP` 与 `Q01_RESTART_SCOPED_PASS`。可见 IsosUI Control 命令 `ui-cd5f2d76-f184-4fd4-84ce-1f4a31696397` 到达 `Completed`（`live/browser-dom-observation.json`）；`live/browser-result.json` 记录相同命令 ID 的公开 bridge 后续结果为 HTTP 200、`Completed`。`live/browser-visible-vision.json` 记录真正渲染的 640×480 Vision 帧、generation 95、JPEG SHA-256 `2415a3115891e19200164708c64f45cedada33b5bddc4e18eb3f3fb0c7d1c7ce`；截图为 `live/browser-visible-control-vision.png`。

`live/q01-matrix.json` 的六组案例为 `camera`、`g06-r03`、`p01-p03-u01`、`p07-u03`、`r06-a03` 和 `migration`。本次运行案例文件包含软件包/资源及拒绝边负例、有界日志游标、R06/A03 故障与重启、显式迁移及篡改证据。P01/P03/U01 在归属方重放后需要单独以选定 `--ui-stage` 调用；`p01-p03-u01-ui-stage.log` 以 `Q01_P01_P03_U01_REPLAY_PASS` 和退出码 0 结束。`live/cases/p01-p03-u01/visible-editor.json` 和 `.png` 记录实际浏览器编辑器保留不可用的 `ghost-device` 并禁用 Save and Run；`p01-p03-u01-verify.log` 记录负例验证通过。`camera.log`、`g06-r03.log`、`r06-a03.log` 及 `migration.log` 均以选定案例通过标记结束。完整离线验证还检查 P07/U03 与案例篡改夹具；`seal-full-verifier.log` 和 `verify-full-verifier.log` 均以 `Q01_FULL_VERIFIED` 结束。

外部评审文件位于 `/home/ashex/projects/Island-eos/logs/test/q04-home-replay-prep/runs/20261103-q04-merged-home-final-5/external/`。冻结 `selection.json` SHA-256 为 `7f5cefa5b6521d42d28b88c916679f2b668ba93fd61bbcbbfed4bb8fcc722b87`。选定构建产生者的原始收据 SHA-256 为 `b428ba0b899683ce4cfb9dfff97c1ead4e198103782e44505c1188faaeca6ed4`；评审人在**归档外**写出规范化 JSON 收据，SHA-256 为 `f20b42a50ca7ce68fb27f057742d1e4cc7951853d50b919c1ac388b151d7d88a`。`build-receipt-canonicalization.json` 记录解析值相同、实时归档字节未改动。外部案例收据 SHA-256 为 `cbfd7cb46bd4ba934933c356611f309e1d547940250f1f2fecf5cb483bf7195b`。归档 3,630 个文件的 `live/evidence.sha256.json` 清单 SHA-256 为 `b3c06413e1c99bbad91c4169c806f853e37be8f7fceb1786558d3ca6ff09e29b`。独立归档评审未发现 P1/P2 问题。

### 复现环境与命令

Final-5 使用 Ubuntu 24.04、MuJoCo 3.16.0、GCC 13.3.0、Node 22.23.4、npm 10.9.9，以及含 `pytest 9.2.0` 和 `pytest-asyncio 1.4.1.post1` 的选定 EGIA 虚拟环境。五个干净的 detached 检出目录和真实的二进制/构建路径均位于 `/home/ashex/projects/q04-review/`；临时文件使用 `/home/ashex/q04t`，避免 Unix 套接字路径过长。EGIA 固定引用的同级 `../Island-eos/share` 包含与 EOS 逐字节一致的 148 个文件（文件映射 SHA-256 `66606a885f81995037ab23e76a3a81e6452d0b0763fbc87354c7b71965399666`）。冻结时 `/home` 可用空间为 491.6 GiB。MuJoCo CMake/CTest（26/26）、IsOS 和 EOS Cargo 构建、EGIA 冻结同步/导入以及 UI `npm ci`、测试（191/191）与两种构建的准备日志位于 `q04-home-replay-prep/build-logs/`；final-5 EOS 重建、软件包预检、冻结、五个远程分支/UI/harness 检查和 release-view 单测位于本次运行的 `build/`。`live/selected-inputs.json` 与 `live/command-argv.json` 记录选定路径和二进制哈希。

被 Git 忽略的评审配方位于 `/home/ashex/projects/Island-eos/logs/test/q04-home-replay-prep/replay.sh`（final-5 包装脚本 SHA-256 `4af0ead664ee2f0d8165f50790be1efb07be5fe2a32d780a9680dc9aca4bf9b4`）；相邻的 `freeze_selection.py`、选定 EOS 的 `tests/m4_affected_replay.py`、六个 `tests/q01_*_replay.py` 程序、`tests/q03_case_receipt.py` 和 `tests/verify_q01.py` 定义各阶段命令。检出上述五个准确提交并准备好软件包后，本次实际阶段顺序为：

可用以下归属方命令准备选定的构建输入；冻结前须保存输出和哈希。经审查的 MuJoCo 打包工具写出下文使用的 `bundle/release.json` 与资源归档。Final-5 的 EOS 重建记录在 `build/eos.log`。

```sh
export TMPDIR=/home/ashex/q04t TMP=/home/ashex/q04t TEMP=/home/ashex/q04t
export MUJOCO_ROOT=/home/ashex/mujoco-3.16.0-linux-x86_64/mujoco-3.16.0
cd /home/ashex/projects/q04-review/mujoco
cmake -S . -B build-q04 -DMUJOCO_ROOT="$MUJOCO_ROOT"
cmake --build build-q04
ctest --test-dir build-q04 --output-on-failure
python3 tools/build-backend-bundle.py --build-dir build-q04 \
  --output /home/ashex/projects/Island-eos/logs/test/q04-home-replay-prep/bundle
cd /home/ashex/projects/q04-review/isos && cargo build --locked --bin isos
cd /home/ashex/projects/q04-review/eos && cargo build --locked --workspace --bins
cd /home/ashex/projects/q04-review/egia && uv sync --frozen
uv pip install --python .venv/bin/python 'pytest==9.2.0' 'pytest-asyncio==1.4.1.post1'
mkdir -p /home/ashex/projects/q04-review/Island-eos
cp -a /home/ashex/projects/q04-review/eos/share /home/ashex/projects/q04-review/Island-eos/share
cd /home/ashex/projects/q04-review/ui && npm ci && npm test && npm run build
```

检出上述五个准确提交并准备好软件包后，本次实际阶段顺序为：

```sh
export Q04_RUN_ID=20261103-q04-merged-home-final-5
export Q04_RELEASE=/home/ashex/projects/Island-eos/logs/test/q04-home-replay-prep/bundle/release.json
export Q04_ASSET=/home/ashex/projects/Island-eos/logs/test/q04-home-replay-prep/mujoco-sim-v0.1.1-linux-x86_64.tar.gz
export Q04_TEMP_ROOT=/home/ashex/q04t
recipe=/home/ashex/projects/Island-eos/logs/test/q04-home-replay-prep/replay.sh
bash "$recipe" preflight
bash "$recipe" freeze
export Q04_AUTHORIZED=1
bash "$recipe" main       # 终端 1；等待 live/browser-ready.json
bash "$recipe" capture    # 终端 2；可见浏览器中观察 Control 和 Vision
bash "$recipe" release    # 终端 2；主进程随后完成 Stop 与回收
bash "$recipe" cases      # 终端 1；保持运行，终端 2 同时执行下述 --ui-stage
```

保持终端 1 的 `cases` 进程运行。在 P01 归属方阶段创建 `live/cases/p01-p03-u01` 后，于终端 2 对该输出运行下述选定 `--ui-stage`。浏览器检查保存 `visible-editor.json` 和 `.png`；须等待终端 1 完成并生成 `q01-matrix.json`，再执行收据和封存命令。单独的通过结果记录在 `p01-p03-u01-ui-stage.log` 与 `p01-p03-u01-verify.log`。`capture` 阶段把首份 `main` 收据复制到归档外；在 `receipt` 前，评审人仅规范化这份外部副本，因为选定验证器要求紧凑 JSON。`build-receipt-canonicalization.json` 与两份收据保留准确的字节与解析值比较。

```sh
cd /home/ashex/projects/q04-review/eos
# 终端 1 运行 cases 时，在终端 2 执行：
python3 tests/q01_p01_p03_u01_replay.py \
  --isos-root /home/ashex/projects/q04-review/isos \
  --mujoco-root /home/ashex/projects/q04-review/mujoco \
  --ui-root /home/ashex/projects/q04-review/ui \
  --eos-bin-dir /home/ashex/projects/q04-review/eos/target/debug \
  --asset "$Q04_ASSET" --release "$Q04_RELEASE" \
  --output /home/ashex/projects/Island-eos/logs/test/$Q04_RUN_ID/live/cases/p01-p03-u01 \
  --selection /home/ashex/projects/Island-eos/logs/test/q04-home-replay-prep/runs/$Q04_RUN_ID/external/selection.json \
  --expect-selection-sha256 7f5cefa5b6521d42d28b88c916679f2b668ba93fd61bbcbbfed4bb8fcc722b87 \
  --ui-stage
# 等待终端 1 的 cases 结束后，继续在终端 2 执行：
python3 - <<'PY'
import json
from pathlib import Path
p = Path('/home/ashex/projects/Island-eos/logs/test/q04-home-replay-prep/runs/20261103-q04-merged-home-final-5/external')
receipt = p / 'q03-build-receipt.json'
receipt.write_text(json.dumps(json.loads(receipt.read_text()), sort_keys=True, separators=(',', ':')) + '\n')
PY
bash "$recipe" receipt
bash "$recipe" seal
bash "$recipe" verify
```

`main` 和 `capture` 须在不同终端执行，并由人观察可见浏览器。本序列描述保留的 final-5 运行；新运行须使用唯一编号、重新冻结选定文件并审查新的外部收据哈希。`seal-full-verifier.log` 和 `verify-full-verifier.log` 保存完整命令输出及两次 `Q01_FULL_VERIFIED` 判定。

从选定 EOS 提交 `0bd28b4d2c3ba90a43b81c02ea4e883ef5043db0` 的检出目录运行以下命令，可复现离线结论：

```sh
python3 tests/verify_q01.py /home/ashex/projects/Island-eos/logs/test/20261103-q04-merged-home-final-5/live \
  --expect-selection /home/ashex/projects/Island-eos/logs/test/q04-home-replay-prep/runs/20261103-q04-merged-home-final-5/external/selection.json \
  --expect-selection-sha256 7f5cefa5b6521d42d28b88c916679f2b668ba93fd61bbcbbfed4bb8fcc722b87 \
  --expect-build-receipt /home/ashex/projects/Island-eos/logs/test/q04-home-replay-prep/runs/20261103-q04-merged-home-final-5/external/q03-build-receipt.json \
  --expect-build-receipt-sha256 f20b42a50ca7ce68fb27f057742d1e4cc7951853d50b919c1ac388b151d7d88a \
  --expect-case-receipt /home/ashex/projects/Island-eos/logs/test/q04-home-replay-prep/runs/20261103-q04-merged-home-final-5/external/q03-case-build-receipt.json \
  --expect-case-receipt-sha256 cbfd7cb46bd4ba934933c356611f309e1d547940250f1f2fecf5cb483bf7195b \
  --expect-isos 76a1d6644ec4e73a0bd47105652d9bbc18a9cae3 \
  --expect-eos 0bd28b4d2c3ba90a43b81c02ea4e883ef5043db0 \
  --expect-egia cca1ce66dc2481e2473b55325bc453da1b61c44c \
  --expect-ui f0d29848b57ba34e2557cc978fb294912064cc5c \
  --expect-mujoco 04b65882cd11b0aff8ce56473164691dbd662196
```

验收结论仅适用于上述五个准确合并集成提交和输入。托管 Origin 检查使用本机回环模拟，并非公网 HTTPS 部署。EOS 集成分支合入 `master`、EOS/EGIA 公开快照关卡、IsOS/MuJoCo/UI 发布评审及选定 0.x 标签仍开放；Q04 保持未勾选。
