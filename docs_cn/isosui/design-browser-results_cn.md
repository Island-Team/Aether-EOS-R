# R07 浏览器结果客户端与工作台状态

[English](../../docs/isosui/design-browser-results.md) · [UI 契约](interfaces/browser-results_cn.md) · [首次使用](usage_cn.md)

**R07 已合并并完成模拟底盘范围验证。** `EosWorkbench.tsx` 挂载 `PrimitiveBaseCard`；该卡呈现一个活动底盘绑定，并管理类型化发送、关联结果读取和状态模型。旧版运动控件不再渲染；U04 后续扩展完整的每绑定 UI。旧控件不能变成另一条运动路径。[EOS 所有者 R07 契约](https://github.com/Island-Team/Island-eos/blob/refactor%28v2%29/runtime-main/docs_cn/eos/interfaces/control_cn.md)规定线路和错误；IsOS 拥有修订、运行和 Stop。

## 客户端与状态边界

R07 实现使用 `src/services/eos/primitive-command.ts` 和 `primitive-result.ts` 生成精确的公开发送／读取请求并解码六种状态。`PrimitiveBaseCard` 记录受管理的运行和修订，在发送前及每次读取前复查 IsOS，限制后续查询，并在 EOS 错误期间保持 IsOS Stop 与当前运行日志入口。运动卡目前仅允许 `mock-cxx` 底盘；MuJoCo 运动仍由 R06 把关。

仓库根目录执行 `python3 tests/fixtures/browser_results/check.py` 通过 46 项，`npm test` 通过 96 项，`npm run build` 在 Node 22 和 npm 10 下生成有效的本地及托管构建。审查修复的完整输出保存在忽略提交的 `logs/test/20261102-r07-ui-review-fix/`；初次实现输出保留在 `logs/test/20261102-r07-ui/`。这是早期实现检查：当时 `127.0.0.1:16000` 的 IsOS 和 `127.0.0.1:16020` 的 EOS 均未运行，`live-probe.log` 记录连接失败。后续独立审查和受管理模拟重放已完成下述 R07 范围验证。审查修复测试覆盖发送中禁止轮询、旧修订后的迟到回复、发送响应丢失后不重发运动、单条在途读取、读取后再次核对 IsOS、隐藏页暂停、60 秒上限、跳过未验证的 MuJoCo 绑定，以及准入的旧版 `@v1.0` 模拟绑定。公开载荷字节测试保留 `0.000001`，不增加 UI 精度限制。

`src/services/eos/primitive-command.ts` 发送范围限定的 `base.velocity` 命令，`src/services/eos/primitive-result.ts` 执行受保护的读取；二者沿用已验证回环地址与请求选项。发送方接收已解析的活动绑定、其唯一图连线目标、活动修订、已校验的 `body_velocity` 参数、EOS 单调时间、正序号和新命令 ID；它只生成公开 `primitive_typed` 封包，以 `TextEncoder` 计算紧凑 JSON 的 `payload_length_bytes`，拒绝不安全时钟／期限数值。它不接受自由目标或私有授权。结果客户端只接受 `{commandId, activeRevision}`，生成恰好 `op`、`command_id`、`active_revision` 三个公开键，解析六状态 `DispatchResult`，核对命令 ID，并把有界传输／所有者／协议错误映射为可辨识结果。两个客户端共用解码器，使首次发送与后续查询的状态语义相同。组件通过这些服务调用，不直接调用 `fetch`。HTTP 拒绝或 `ok:false` 响应是错误，不能强行转成命令的 `Rejected` 状态。服务超时或损坏的成功响应是不可用，不是 `OutcomeUnknown`，因为只有 EOS 能赋予命令该状态。

`src/components/isos/primitive-result-state.ts` 提供纯状态模型，由 `PrimitiveBaseCard` 使用。记录包含发送时捕获的 IsOS `runId`、`activeRevision`、活动绑定路径、生成的 `commandId`、最后一个所有者 `DispatchResult` 或 null、浏览器阶段（`sending`、`following`、`terminal`、`paused`、`unavailable`）、最后的安全错误及请求代际。只有 EOS 返回关联的 `Completed`、`Failed`、`Cancelled`、`Rejected` 或 `OutcomeUnknown`，状态才可转为 `terminal`；`InProgress` 保持 `following` 或 `paused`。状态模型区分 `Failed`、`Rejected` 与传输错误，遇到错误仍保留命令 ID，发送超时或结果不确定后也不再提交第二次 `primitive_typed`。

R07 呈现一个最小活动 base 绑定卡，显示图解析目标及四个有界 SI 字段，由操作员点击 Invoke。这张已验证的卡仍仅限模拟适配器；MuJoCo 运动继续由 R06 把关。发送前解析精确的活动图绑定与目标，检查 `saved_revision === graph.revision === active_revision`、`run_id_state:known`、`state:running`、无未保存草稿、EOS 就绪以及所有者报告的目标运动可用。卡片只捕获一次运行／修订／绑定／命令 ID，取得 EOS 单调时间，在发送前立即复查 IsOS 围栏，然后只提交一次。状态模型在每次后续读取之前、页面重新可见之后重新读取 IsOS `process-status`，比较当前 `run_id` 和修订。IsOS 状态读取失败或 `run_id_state:unavailable` 时暂停后续查询并标为不可用。运行停止、运行 ID 或修订变化时取消未完成读取，并把记录封存为旧运行／不可用。浏览器请求代际与 `AbortController` 防止选择变化、Stop、卸载或运行改变之后的迟到回复更新当前卡片。即使选中相同绑定，新命令也建立新记录。UI 从 IsOS 读取活动修订，不从草稿或命令响应推断。

结果轮询一次只读一个请求，遵循 UI 契约中的一秒初始延迟、十秒错误退避上限、六十秒／十二次上限及隐藏页暂停。达到上限时 `InProgress` 仍可见；手动刷新先重新检查 IsOS，再执行一次读取。终态停止读取。若首次发送响应丢失，UI 只能用生成的 ID 进行这种受保护的结果读取，把 `unknown command ID` 视为不透明不可用。服务层绝不重发 `primitive_typed`。Vision 仍使用独立的只读 WebSocket 和修订围栏。结果卡显示命令 ID、活动修订、绑定路径、精确所有者状态或不可用解释和最近更新时间。只有 EOS 所有者事件提供对应的有效 `correlation_id`，才可预填日志筛选；否则打开无筛选的当前运行日志。不能假设命令 ID 就是日志关联 ID，也不能用 Graph 连线动画当作运动释放证明。

后续受管理的跨所有者重放使用 IsOS master `1db1aa5`、EOS R07 `60e4c16`、IsosUI R07 `202df06`、全新家目录与单个 `mock-cxx` 底盘。验证、带修订检查的 Apply、启动、操作员点击 `base.velocity`、EOS `OutcomeUnknown` 及日志记录、IsOS Stop、重启，以及当前运行的 `Rejected` 结果读取均已观察到。首次释放结果不确定后，EOS 保留运动禁用状态；新运行中被拒绝的命令没有重发。重放发现 UI 缺陷：重启后旧运行的终态 `OutcomeUnknown` 没有旧运行标记。运行围栏修复现在同时封存终态和非终态记录，保留旧所有者状态与命令 ID，并仅在新运行围栏满足后允许新命令。挂载组件回归测试覆盖运行轮换及 Stop 时迟到的读取。此模拟重放不证明 R06 的 MuJoCo 物理停止。原始证据和可复现命令位于 EOS 检出目录中被忽略的 `logs/test/20261102-r07-cross-owner-live/SUMMARY.md`；本次 UI 修复的完整检查输出位于被忽略的 `logs/test/20261102-r07-ui-run-fence/`。

## 与当前工作台集成

`EosWorkbench` 每五秒刷新 IsOS 状态以显示修订警告；`PrimitiveBaseCard` 在每次结果读取前获取新状态，读取后再次核对。类型化客户端区分所有者、传输和协议错误，同时维持已验证地址、CORS 模式及无凭证策略。桥接失败时命令卡保持挂载并标记不可用；后台刷新不清除命令记录。六种状态均有中英文文本，并按情况使用 `role=status` 或 `role=alert`。`Failed`、`Rejected`、`OutcomeUnknown`、旧 ID 不透明错误及桥接离线要区分显示；不能只靠颜色。R07 卡在 Invoke 旁调用 IsOS `stopProcesses()`，独立于 EOS 桥接、图可编辑性和结果轮询。它解码类型化的 `{stopped,terminal}` 响应并显示与运行 ID 匹配的 Stop 结果；R06 拥有物理停止回执。完整活动绑定列表、丰富的类型化参数表单、信息卡和浮动面板布局仍属 U04/U03。

## 审查与验证门槛

[夹具矩阵](../../ui/tests/fixtures/browser_results/cases.json)固定类型化发送、结果读取、所有者结果、运行／修订、离线、日志入口与 Stop 转换。模拟 EOS 桥接和 IsOS 状态服务的测试验证精确公开请求体、无授权、唯一活动绑定与目标、载荷字节长度、安全单调期限、六种状态、运行变更后的迟到响应、首次发送超时但不重发运动、一次仅一个读取、隐藏页暂停、六十秒上限、`unknown command ID` 不透明、桥接 502/503、修订过期、损坏结果及 IsOS Stop 可用性。挂载浏览器组件测试证明 EOS 离线且活动修订不符时 Stop 仍可用。当前 `npm test` 及 `npm run build` 检查覆盖本地与托管构建。跨所有者首次使用重放使用受管理模拟底盘的 IsOS 保存／活动运行、EOS 桥接、真实 `primitive_typed` `InProgress` 响应与受保护结果读取，随后执行 Stop 并查看当前运行日志。真实 MuJoCo 运动仍由 R06 把关。R07 范围验证现已在合并的所有者版本上完成；完整逐绑定工作台 U04 仍未完成。

此修订设计运行 `python3 tests/fixtures/browser_results/check.py`，46 个唯一发送／读取／状态用例与六种状态均通过；相对链接检查覆盖十二个修改的中英文设计及夹具页面；`git diff --check` 也通过。完整输出位于被 Git 忽略的 `logs/test/20261102-r07-isosui-design-revision/`。这是实现前的设计检查记录；后续独立设计及实现审查均已通过。

## 合并版本 R07 验证

EOS [#323](https://github.com/Island-Team/Island-eos/pull/323)、[#324](https://github.com/Island-Team/Island-eos/pull/324) 与 IsosUI [#37](../island/retired-repository-references_cn.md#ref-bcef1a59120a)、[#38](../island/retired-repository-references_cn.md#ref-9028ce737965) 通过独立审查及合并后范围验证。[共享 R07 记录](https://github.com/Island-Team/Island-eos/blob/refactor%28v2%29/runtime-main/docs/island/implementation-todo.md)固定了合并版本和带校验和的重放目录。一次受管理模拟底盘 UI 点击以同一命令 ID 经历 `Sending`、`InProgress` 和持久化 `OutcomeUnknown`，产生一条日志记录。公开结果所有权及拒绝边界、桥接离线、旧运行不透明、`Releasing` 期间 Stop、重启后不重发、同运行 Stop 确认显示与结果围栏均通过。最终 UI 重放用真实 IsOS 验证已确认 Stop 和传输故障；明确的所有者拒绝使用与 IsOS HTTP 422 响应完全相同的桩。未知 Stop 终态由挂载组件测试覆盖，未在模拟图实时诱发。UI 检查通过 113 项测试和两种构建。R06 仍负责 MuJoCo 物理停止，U04 负责完整逐绑定控件，M5 负责完整发布首次使用重放。

## Stop 终态显示修正

R07 实际重放发现 EOS 卡从 IsOS Stop `{stopped,terminal}` 响应中读取不存在的 `result.state`，工作台也将该响应当作 `ProcessStatus` 保存。独立的 `stopProcesses()` 服务类型消除了这两种混淆。共用的运行时解码器在显示确认前验证完整终态、固定原因代码和点击时捕获的运行 ID。Home、Graph、EOS 对等待、确认、未知、未确认及响应不可用使用相同语义。EOS 卡点击 Stop 时立即隔离未完成的结果读取；各 Stop 路径在响应后重新读取 `/process-status`，状态刷新失败也保留 Stop 结果。组件测试覆盖 EOS 离线与修订过期、迟到结果、未知及传输故障；纯测试覆盖 null、缺失、损坏和运行不匹配的终态证据。完整检查命令及输出位于被 Git 忽略的 `logs/test/20261102-stop-terminal-display/`。

独立审查还要求重复的终态子进程 ID 必须拒绝确认、`terminal:null` 且 `stopped` 非空必须保持未确认、明确的 `IsosApiError` 拒绝须保留所有者身份。这些情况在共用解码器和挂载 EOS 卡中有负向测试；三个 Stop 入口共用相同的错误呈现。
