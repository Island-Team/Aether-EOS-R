# Q03 IsosUI 验证设计

[English](../../docs/isosui/q03-qualification.md)

**状态：**设计已审查；选定的已安装浏览器证据在封存的 Q03 final-17 回放中通过，合并后提交的 Q04 final-5 重放也通过独立归档审查。IsosUI `v0.1.0` 已于随后发布；[已批准的发布资产 run-7](https://github.com/Island-Team/Island-eos/blob/refactor%28v2%29/runtime-main/docs/island/q04-merged-qualification.md)已完成既定范围的 Q04/M5 跨所有者发布关卡。公网 HTTPS 浏览器首次使用仍待验证。IsosUI 呈现所有者报告的图、状态、日志、视觉数据和原语结果。IsOS 拥有图 Apply 与当前运行事件；EOS 拥有命令准入。编辑器不得把图连线当作浏览器命令执行。

## 源码与静态检查

主题分支从已获取的 IsosUI `master` 创建，但构建和验证须使用经过审查的精确代码／契约主题 HEAD；记录 master 基点与选定候选提交、另外四个所有者提交、Node 22/npm 10、锁文件 SHA-256 以及构建清单／资源哈希。`scripts/verify-bundles.mjs` 的 `source_commit` 必须为该精确代码／契约 HEAD，不可用旧 master 基点替代。数值基准证据须提交到基于已验证代码提交的单独文档主题，使选定包与代码主题 HEAD 保持相同身份。PR 前重新检查远端 master，Q04 再验证合并后的提交。运行 `npm ci --no-audit --no-fund`、`npm run typecheck`、`npm test`、`npm run build`、两个包的清单检查及 `git diff --check`。确认本地 `/` 和托管 `/isosui/` 资源前缀、`source_commit`、版本 `0.1.0`、schema 与目标。运行浏览器结果和 Q01 Vision 契约检查器，以及目录、连线、面板、日志游标、原语期限／结果／Stop 和视觉来源的定向测试。负面用例包括过期图修订、不支持的图／类型、不可用选择项、旧运行响应、错误流／门／来源、畸形帧和所有者不可用。将每项命令、退出码及完整输出保存到忽略 Git 的 `logs/test/<run-id>/`。

## 安装后的首次使用

在全新的 IsOS 主目录中安装已验证的 `island/mujoco-sim@0.1.1`，提供选定的本地界面包。在真实浏览器访问 `/en/isos` 与 `/cn/isos`，选择场景／设备，Validate 并按修订 Apply，查看已保存／活动修订及就绪、类型／实例描述和连线诊断。操作独立的 Graph、Control、Monitor、Vision、Logs 面板，检查焦点和键盘、44 像素触控目标、窄视口、两种主题、布局持久化，以及 EOS 不可用时 Stop 的可见性。选择一个活动底盘或机械臂绑定，验证精确目标与 SI 字段帮助，调用一次，并在 Sending、InProgress、终态／OutcomeUnknown 全程保留命令 ID。将结果与当前运行日志关联；注入故障／重启并证明命令未重放。检查过期修订、桥接不可用、图像拒绝和已确认／不确定 Stop。测试显式旧图迁移与不支持图的只读行为。保存截图或 DOM 断言及所有者 HTTP／事件记录，并去除授权、提示和媒体敏感值。

托管检查须把 `/en/isos` 和 `/cn/isos` 映射到精确的 `dist/hosted/index.html`，仅在 `/isosui/` 提供托管资源，拒绝回退到 `dist/local`。在实际浏览器断言已加载清单及每项观测到的 JS／CSS 响应 SHA-256 与选定托管文件一致，包括在 Graph／Control 交互期间加载的懒加载模块；保留完整观测摘要映射，并拒绝后期替换的模块。然后验证相同编辑器 URL 与环回 API 策略。选定的 Q01 验证器须确认同一运行、修订、门／来源、渲染设置和流的 IsOS `vision_evidence` 与 EOS `subscribed` 证明；按规定与 EGIA 比较已接受 JPEG 的摘要及代次。界面交付帧率与跳过代次仅是浏览器观察值，不能当作 EOS 队列指标。单元用例不能单独关闭安装关卡。

## 浏览器基线

三次重复测量各自启动新的 Firefox 与 geckodriver 会话，重新校验选定的生产构建包，并记录浏览器版本、会话 ID、驱动进程 ID 和已观测资源哈希。每次会话分别执行每项 100 次预热和 1,000 次计时操作，保持 2 秒交互期限及遇错立即保存检查点。浏览器启动、上一会话退出或资源校验失败时，也须在公布本次运行前保存已完成用例的检查点。预热失败仍算失败；之后通过的重试不能把未完成的运行算成基线证据。先前共用长期浏览器会话的本地和托管失败尝试保留在 `logs/test/20261103-q03-ui-fecafe9/` 供诊断，二者均在较晚的 Control 交互中、发送 EOS 命令前停滞。

2026-11-03 的进程重置验证：`npm run typecheck`、`npm test`（191 项通过）、`npm run build`（两种构建包与清单）、`python3 -m unittest tests/q03_browser_benchmark_test.py -v`（10 项通过）、`python3 tests/fixtures/q03_browser_benchmark/v1/check.py` 和 `git diff --check` 均通过。本地与托管目标还分别以 `--warmup 1 --samples 2 --repetitions 3` 通过隔离模拟服务的小规模检查：每个目标有三个不同的浏览器会话和驱动 PID，每次重复有五项已校验资源摘要，失败为零。完整命令与输出位于忽略 Git 的 `logs/test/20261103-q03-ui-process-reset/`；小规模检查只验证运行器生命周期，不代替完整基准负载。

测量前提交 `tests/fixtures/q03_browser_benchmark/v1/`，其中包含从经脱敏的 Q01 选定图得到的精确 19 节点、10 条输入连线图、一个活动底盘绑定、桩 JSON 响应体及各响应的 SHA-256／字节长度清单。夹具固定 5 毫秒响应延迟、合成命令 ID 的两步 `InProgress` → `Completed` 结果、每次交互 2 秒超时和 1280×800 浏览器视口。生产环境的 Control 结果只有 Invoke 后才会出现，因此浏览器每个样本仅可向隔离的 EOS 基准桩发送一次合成 `primitive_typed` POST。运行器必须先独占固定的 IsOS、UI 和 EOS 环回端口，若端口已被占用则拒绝启动；检查目标、修订和载荷，并断言命令 ID 唯一且没有重试。仅当提交的 POST ID、每次授权结果读取 ID 和可见终态 ID 相同，且活动修订与 IsOS 运行身份与该样本相符时才计入 Control 样本；过期或旧运行结果必须拒绝。请求不得路由到真实所有者或物理执行器。分别测试固定的本地与托管生产包：每个包预热 100 次，再串行执行 1,000 次 Graph 打开／目录渲染和 1,000 次 Control Invoke／结果显示。Graph 计时从导航到 `?page=graph` 前立即开始，仅在选定修订和 19 节点／10 连线可见且跨两帧稳定后结束。Control 计时从导航到 `?page=eos` 前立即开始，仅在同一运行的绑定与终态结果跨两帧稳定后结束。每个样本之前须回到前一页面，等待所选修订和可见内容稳定，再开始计时。只有本次 Invoke 产生合成 POST ID 后，才能接纳终态 Control 结果；上一张控制卡的结果不能完成新样本。关联或 POST 数量检查失败时立即停止，并用失败检查点保留已尝试的结果与请求，以及该轮已完成的测试项。用浏览器单调时钟计时，保留每次延迟、结果、实际响应大小及测得的墙钟时长。定向测试须证明计数、拒绝／失败响应和超时行为。每个包运行三次，逐次及合并报告完成操作数／秒、中位数（偶数取中间两数平均）及最近秩 p99（毫秒）、失败数和顺序。记录浏览器、操作系统／CPU、包哈希与图规模。把桩浏览器延迟与真实 MuJoCo 或 Dispatcher 延迟区分开。测得基线前不设阈值。独立设计审查须先于测试工具实现，独立实现／证据审查须先于面向 IsosUI `master` 的 PR。
