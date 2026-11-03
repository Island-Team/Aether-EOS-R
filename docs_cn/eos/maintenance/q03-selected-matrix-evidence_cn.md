# Q03 选定 Q01 矩阵证据

[English](../../../docs/eos/maintenance/q03-selected-matrix-evidence.md)

Q03 使用五个选定主题提交验证完整 Q01 重放。历史 `live-3` 归档保留为独立的固定配置。下文 final-17 选定归档在修正有界日志游标重放后通过完整离线验证；合并后提交与发布验收属于 Q04。

审查者控制的选定文件是候选归档之外的规范 UTF-8 JSON。其 schema 版本为 `1`，配置为 `selected-q03`，包含准确的 `isos`、`eos`、`egia`、`ui`、`mujoco` 五个所有者，以及各自的规范仓库 URL、主题引用、提交和根树、相关子树、锁文件和源文件哈希、所有执行的测试程序哈希、完整的 UI 本地和托管构建输入映射、MuJoCo 发布文件和资产哈希。运行前，操作者在另一份记录中冻结该文件的 SHA-256。实时重放在启动前验证干净的 `HEAD`、远端引用、所选提交中的 Git 树和锁文件内容、完整 UI 构建输入映射及测试程序字节。运行生成二进制文件和两种 UI 构建的不可变构建收据；审查者在构建后将其复制到归档之外并计算哈希，验证程序接受外部文件及其摘要。构建输入映射必须覆盖 `src/`、`public/` 和 `scripts/` 下所有已跟踪文件，以及包、锁文件、Webpack 与 TypeScript 配置文件。

选定配置的完整验证需要 `--expect-selection`、`--expect-selection-sha256`、`--expect-build-receipt`、`--expect-build-receipt-sha256`、`--expect-case-receipt`、`--expect-case-receipt-sha256` 和五个 `--expect-<owner>` 提交。审查者在六次案例运行后才冻结规范案例收据。`tests/q03_case_receipt.py` 从六个案例清单和 UI 构建在归档之外准备候选文件；审查者检查并冻结其摘要，操作者在封存根归档前将该文件准确复制到 `q03-case-build-receipt.json`。收据包含选定 SHA-256、主构建收据 SHA-256、每个案例清单 SHA-256 和准确的案例专用 UI 文件哈希。验证程序将每个适用案例的 IsOS、EOS、EGIA 可执行文件与主收据比较，将保留的包资产与主收据比较，并将所有案例专用 UI 文件与外部案例收据比较。没有外部文件的新五提交组合会被拒绝。旧的五参数调用只接受历史组合。`--main-only` 仍是不完整的诊断。重新生成校验清单不能授权变更后的证据。

选定主重放分别构建 `dist/local` 和 `dist/hosted`，记录实际提供的本地首页和脚本，以及托管 `/en/isos` 与 `/cn/isos` 的 HTTP 字节哈希；托管资源位于 `/isosui/`。在浏览器暂停期间，`tests/q03_browser_capture.py --archive <live-output> --selection <external-selection> --expect-selection-sha256 <digest>` 在 Firefox 导航前启动本地记录代理，保留实际发送到浏览器的初始导航和所有观察到的 JavaScript/CSS 响应 URL、状态及 base64 字节。`local`、`hosted.en` 和 `hosted.cn` 的采集覆盖 Graph 与 Control 页面导航以及本地 Vision 面板打开，并等待资源集合稳定；同时保留浏览器导航计时、资源计时、`document.scripts` 和原始编辑器 DOM。程序还在浏览器中显式获取每种构建的 `manifest.json`，保留浏览器正文与截获响应。离线验证程序匹配浏览器资源 URL 与截获响应，核对选定构建收据中的字节，并检查清单 schema、版本、选定 UI 提交和目标。当前 Webpack JavaScript 通过 `style-loader` 包含 CSS；若观察到外部 CSS 响应，也须核对。代理只保留获准的首页、JS、CSS 和清单正文，不归档无关的所有者 API 响应正文。

托管浏览器采集是**模拟托管 Origin 检查**，因为选定的托管页面由环回测试服务器提供。记录代理仅在请求 Origin 准确等于该测试服务器时，向选定 IsOS 和 EOS 桥接端口上的 `/api/isos/v1/` 与 `/api/eos/v2/` 路由转发 `https://os-island.com`。只有上游原始 `Access-Control-Allow-Origin` 准确批准 `https://os-island.com` 时，代理才向浏览器返回合成 Origin；原始状态保持不变。`q03-simulated-hosted-origin.json` 逐请求记录原始与转发的 Origin、上游与交付状态及 CORS 标头，并记录未经代理转换的 IsOS GET 和 EOS 预检对合成 Origin 返回的 403。该行为仅属于测试代理，不改变生产 CORS 策略，也不证明公网域名部署。

选定重放必须在浏览器暂停解除前取得该记录。离线验证程序检查准确端口、API 路由、Origin 映射、上游批准的 CORS 标头、未变更的成功状态、IsOS GET、EOS Control 的 **POST 且 `content-type` 预检**、上游允许的方法及标头，以及两项直接 403 检查。如果 Control 页面导航未产生该预检，采集程序会经代理执行浏览器 Origin 的 `POST {"op":"time"}` 并要求其预检，同时记录证据来自页面导航还是显式浏览器探测。`q03-hosted-origin-validation.json` 必须与记录摘要和验证结果一致；归档校验清单包含两个文件，审查者控制的案例收据冻结记录摘要。缺失或矛盾的记录即使在本地重新封存校验清单后仍会失败。

集中测试命令 `python3 -m unittest discover -s tests -p 'test_q03_*py' -v` 中有五项通过，包括缺失及矛盾 Origin 证据以及 EOS 预检为 GET 或标头不完整时的拒绝测试；本地 Python 缺少 Selenium，真实 Firefox 桩测试被跳过。收据测试命令 `python3 -m unittest discover -s tests -p 'test_q01_selection.py' -v` 中有十项通过。完整输出位于被忽略的 `logs/test/20261103-q03-hosted-origin-proxy/strict-preflight.log` 和 `selection-preflight.log`。后续 final-15 采集使用了真实 Firefox。

采集程序在默认 Graph 布局加载后，通过实际 UI 打开 Vision 工作面板。随后，它在浏览器暂停期间采样所有者套接字、桥接 WebSocket 和 EGIA 检查套接字，采集浏览器当前 blob JPEG 与 DOM 元数据，并在有界次数内重试，直到浏览器 JPEG 与新采样的所有者摘要及代次相符。归档中的 `q03-vision-*` 观察和 `q03-browser-raw.json` 会一并接受离线检查；暂停前的帧不作为固定目标。操作者还写入现有的本地 Control 观察与结果以及派生的托管和 Vision 摘要。本地 Control 结果仍需满足 EOS 终结日志记录和当前运行日志的绑定条件。

对本地及托管采集，程序点击 EOS 页面，并等待所选导航状态、可见 Control 区域与卡片、选定活动修订、绑定路径及可用的 Invoke 控件，再返回 Graph。它保留 Control DOM 与 URL，并持续记录直至延迟的 JavaScript/CSS 资源稳定。`final-control-stub.log` 证明隐藏的 Control 区域不能在延迟分块到达前完成采集，且被替换的延迟分块会被拒绝；`final-control-focused.log` 记录 24 个集中测试通过。final-15 选定重放也覆盖了这一浏览器边界。

浏览器修订的检查通过：`rereview-focused.log` 中 24 个集中 Q01 测试、`rereview-live-stub.log` 中两个代理和帧推进桩测试，以及 `rereview-firefox-stub.log` 中一个有界真实 Firefox 测试。Firefox 测试覆盖本机代理路由、初始加载字节、清单获取、WebSocket 转发、Vision 面板打开和 blob 采集。集中测试拒绝被替换的初始或后续脚本、错误清单及无关 API 正文保留。完整日志位于同一忽略目录 `logs/test/20261103-q03-selected-matrix-focus/`。下文 final-15 选定六案例重放已通过。

审查修复后，对 `/home/ashex/projects/Island-eos/logs/test/20261103-q01-full-current/live-3` 的历史只读完整验证通过，结果为 `Q01_FULL_VERIFIED`。完整输出位于忽略目录 `logs/test/20261103-q03-selected-matrix-focus/review-fixes-legacy-final.log`；同目录的 `review-fixes-focused-final.log` 记录 23 个集中 Python 测试通过，`review-fixes-main-tamper-final.log`、`review-fixes-p01-tamper-final.log`、`review-fixes-r06-tamper-final.log` 记录现有三组篡改测试通过。完整验证命令为：

```bash
python3 tests/verify_q01.py /home/ashex/projects/Island-eos/logs/test/20261103-q01-full-current/live-3 --expect-isos 00c1d7aa184de8b969e6e40472bf9b1fa55032ff --expect-eos 642166576a2cddfd97cd703e5bd9e53567745515 --expect-egia d3868d2763c35d9f6bfed80d75edb7a1844ef54e --expect-ui 920995788d8e29dd6ab594d31278033bb791cfeb --expect-mujoco 4e6b5449ccd25be53ad95b13baa2c89f7ed91e10
```

选定主运行在就绪后从 EOS 所有者套接字、浏览器桥接 WebSocket 和 EGIA 检查套接字采集同一代次的 Vision 帧，记录图和投影修订、流和源帧 ID、门控代次、渲染设置及已验证的 JPEG 摘要。浏览器操作者写入 `q03-browser-vision-observation.json`，包含 `metadata`、`jpeg_verified_sha256`、`rendered`、`active_revision` 和 `source_commit`；离线验证程序将其与三方所有者观察、选定 UI 提交、原始浏览器 DOM 和浏览器收到的 JPEG 字节比较。

final-15 选定运行在归档之外保留选定文件和审查收据，通过 `tests/m4_affected_replay.py --q01-full` 运行，并在 `q01-matrix.json` 中绑定全部六个案例清单及选定摘要，使用外部收据通过完整离线验证。后续源码或测试程序变更须重新冻结选定文件并重演，才能对变更后的提交声称验收通过。

R06/A03 案例的初始就绪探测将每次 HTTP 响应及观察时间写入 `readiness-probes.jsonl`，并在 `readiness-probes-summary.json` 中记录计数和固定的 360 秒期限。就绪前仅允许 IsOS 文档规定的 `503` 忙碌或断开响应正文。单次 HTTP 请求最多等待三秒，以覆盖所有者的两秒管理器期限，但不能超过整体剩余期限。成功正文必须包含 `stopped` 或 `running` 状态、有效的活动修订和进程条目；`starting` 与 `degraded` 会立即失败。其他错误、格式错误的成功正文以及 `isos run` 提前退出会立即失败；之后对降级和恢复状态的检查仍严格执行。`PYTHONPATH=tests python3 -m unittest -v test_q01_r06_readiness test_q01_harness_guards` 的 20 项集中测试通过；完整输出位于被忽略的 `logs/test/20261103-q03-r06-readiness/unit-state-review.log`。final-15 选定 R06/A03 案例已通过。

管理器重启后，`manager.sock` 可能早于前台 HTTP 监听器出现。案例现在仅在服务进程仍存活时重试连接被拒绝错误，最长 25 秒。首个 HTTP 响应必须是成功的 `200`；HTTP `503`、格式错误的响应、其他传输错误或服务进程退出会立即失败。案例在 `manager-restart-http-probes.jsonl` 及其摘要中保存每次尝试和耗时。`PYTHONPATH=tests python3 -m unittest -v test_q01_r06_restart_http test_q01_r06_readiness` 的 19 项集中测试通过；完整输出位于忽略目录 `logs/test/20261103-q03-r06-restart-http/unit.log`。final-15 选定实时重放覆盖了该路径。

R06/A03 归档验证程序现在将规范化的绝对 `--output` 启动目录与记录的选定二进制及 UI 父目录准确比较。因此，未改动的案例复制到矩阵归档后仍可验证，而不同或同级的启动目录会被拒绝。`PYTHONPATH=tests python3 -m unittest -v test_q01_r06_archive_path` 的两项集中测试通过；对现有选定诊断归档执行 `python3 tests/verify_q01_r06_a03.py logs/test/20261103-q03-selected-final-13/r06-smoke2 --expect-selection /tmp/q03-selected-review-20261103-q03-selected-final-13/selection.json --expect-selection-sha256 8ebb24577e377bc7b42729ada09df334632613ce0e768ebf9e16842ac4c9e582` 也通过。完整输出位于忽略目录 `logs/test/20261103-q03-r06-archive-path/`。此诊断结果不代表六案例完整验证。

## final-15 选定主题提交结果（2026-11-03）

已封存归档为 `logs/test/20261103-q03-selected-final-15/live/`。审查者冻结的选定文件为 `/tmp/q03-selected-review-20261103-q03-selected-final-15/selection.json`，SHA-256 为 `116aef2846750a96275645e4dec3021ad716ce6fb80b05c087a2ce377a74b990`；外部主构建收据为 `977293e097779f636cb70bde9f3d5657bc9351b0b9f67534ca7beb41a97ef915`，外部案例收据为 `20b2dc444d4f90ca1e4cf4b8a2404893c53cd20de19a090ce02debf0c539ab5d`。选定源码提交分别是 EOS `5d8f81f771bf044c4fcf322b77af49816ee1b1f5`、IsOS `b625749c38f056736a1d6dd38759e385c9e04b9e`、EGIA `2e7abae02d6538ba9e2519d6a42eb9ca184c34ec`、IsosUI `90579a9e86033f4180d81d93a212e7b8a61c1850`、MuJoCo `3bf8a50aa2100b3eb1758ea4795bdcdddb65391e`。这些是主题提交，不是已合并的发布提交。

`main.log` 以已确认的 `M4_STOP` 和 `Q01_RESTART_SCOPED_PASS` 结束。真实 Firefox 采集保留本地及托管英中页面加载字节、活动原语绑定、可见 Control 结果及同代 Vision 证据。命令 `ui-1fb70aba-7a11-4ca7-9b7e-738093ee07b3` 的浏览器结果为 HTTP 200 `Completed`；验证器将其与本次 EOS journal 和日志关联。选定 Vision 帧为第 29 代、640×480，JPEG SHA-256 为 `f3ff9f047ca4b88a0ad97d79f6097e68f17ca62a6dc160efdb1337a28961ce2b`。托管 Origin 检查使用本机代理模拟 `https://os-island.com`，不证明公网 HTTPS 部署。

`q01-matrix.json` 中六个案例为 `camera`、`g06-r03`、`p01-p03-u01`、`p07-u03`、`r06-a03`、`migration`。案例日志和完整离线验证记录了成功，包括显式迁移、未知结果时的停止与回收、Restart 边界。`seal-full-verifier.log` 和 `verify-full-verifier.log` 均以 `Q01_FULL_VERIFIED` 结束，后者为可复现的最终结论。选定干净 home 最终为 `stopped`，EGIA、config、bridge、EOS 均为确认的终态，`q01-cleanup-proof.json` 无残留进程。选定文件、收据和案例清单哈希仅证明这些准确字节，不覆盖后续源码提交或 Q04 合并提交。

完整离线验证命令及五个准确提交见[英文证据页](../../../docs/eos/maintenance/q03-selected-matrix-evidence.md#final-15-selected-topic-head-result-2026-11-03)。

## final-17 选定主题提交结果（2026-11-03）

已封存归档为 `logs/test/20261103-q03-selected-final-17/live/`。外部选定文件 SHA-256 为 `35dd4e5aa4c209bf84935002a4eded756f3eb5a1c147b2fb12bd5d894e46b78a`；外部主构建收据与案例收据的 SHA-256 分别为 `794c6a48d9df01e863fc579642167a6a6f2db1ef7a3467fab5f9a00078b2668e` 和 `20585dfe0e68e437b2097b010143c7b8a6092fff7acbb628888d3b2bee56aef5`。EOS 主题提交为 `219f031bae9dbfc9d9f53f62c910b9e235bb2138`；其他四个主题提交与上文 final-15 相同。这组准确提交与外部收据共同限定归档的验证范围。

浏览器可见的底盘控件以 0.1 m/s、250 ms 租约发出命令 `ui-2d91844d-d256-4add-ab8b-f52071a33b6b`。IsosUI 显示 Sending 后转为 Completed，EOS 桥接结果回查对同一命令返回 HTTP 200 Completed。选定的 640×480 Vision JPEG 为第 363 代，SHA-256 是 `5baae133f53fb4ca8aea5fcaf94754ce8cfebd05666ffff6521fb2629796e74b`。托管 Origin 证据仍是本机对 `https://os-island.com` 的模拟，不证明公网 HTTPS 部署。

final-16 暴露了重放程序的分页错误：归属方事件已包含浏览器结果，但某个 64 KiB 过滤游标页没有匹配行。修正后的程序保留每页原始 API 响应和请求游标，检查运行范围、修订、锚点与诊断，并推进至游标稳定。final-17 的任务和浏览器关联各读取三页，分别找到四行和两行，`q01-log-correlation-gaps.json` 为空。`python3 -m unittest discover -s tests -p 'test_q01*.py' -v` 的 50 项聚焦测试通过，覆盖空但继续推进的页以及被篡改游标索引的拒绝；完整输出位于 `logs/test/20261103-q03-selected-final-17/focused-q01-50.log`。这是测试程序修正，IsOS 的有界游标契约没有改变。

`main.log` 以确认的 `M4_STOP` 和 `Q01_RESTART_SCOPED_PASS` 结束。六个矩阵案例 `camera`、`g06-r03`、`p01-p03-u01`、`p07-u03`、`r06-a03`、`migration` 均通过；保存图中的不可用设备保持可见，阻止 Save 和 Run。显式迁移篡改测试通过。`seal-full-verifier.log` 与 `verify-full-verifier.log` 均以 `Q01_FULL_VERIFIED` 结束。归档校验清单 SHA-256 为 `f86973093cc8792aef9cd8773eb222a2aff3142b0f99204389c2cc008ac6cc8f`。本次仅验收选定主题提交；Q04 还须重放合并后提交并完成发布门槛。

完整离线验证命令和五个准确提交见[英文 final-17 证据](../../../docs/eos/maintenance/q03-selected-matrix-evidence.md#final-17-selected-topic-head-result-2026-11-03)。
