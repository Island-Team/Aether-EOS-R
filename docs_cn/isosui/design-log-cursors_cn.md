# 日志游标面板设计——P07

[English](../../docs/isosui/design-log-cursors.md) · [UI 契约](interfaces/log-cursors_cn.md) · [工作台设计](design_cn.md)

现有 `IsosLogsPanel` 使用单个可变游标，每 1.2 秒轮询 `/logs`，遇到 `rotated` 即清空行。`LogLine` 类型仅有时间、级别、来源和消息。P07 扩展 `src/services/isos/node-graph.ts` 的服务适配器及纯面板状态规约器；React 组件不拼接日志 URL。IsOS P07 提供首事件及绑定运行的游标语义。浏览器控制台行保留在独立区域，不能按时间戳混入权威服务端流。

## 状态机

从可选当前日志响应取得 IsOS `scope:{kind:manager_session|deployment_run,run_id,active_revision}`，并带 `selection_generation`。状态还保存 `{anchor,anchor_state,query,lines,cursor,gap,follow,connection}`。停止或失败的部署、PID 丢失、活动修订清空及管理器重启都保持 IsOS 持久化的 `last_run_id` 范围；只有准入新运行才改变。P07 没有清除端点。初次或改变范围时开始新代际，取消旧请求，清空其详情行和游标，再请求 `view=current` 页面，其中锚点不受筛选影响。锚点显示于筛选器上方。锚点待定时 `event=null` 且详情 `lines=[]`；沿同一绑定游标轮询并显示等待。找到后，同一响应携带严格晚于锚点的详情行；来源／级别／节点／关联筛选仅作用于这些行。改变筛选只重置该查询的游标和详情行，不能隐藏或覆盖锚点。每个异步结果携带代际及精确选择；旧运行的迟到响应被忽略。初次分页前先读取状态，重连或范围改变后再核对；绝不从 PID 或活动修订选择日志范围。

进程状态还报告 `run_id_state:none|known|unavailable`。只有由 IsOS 持久 never-run 标记证明的 none 才允许管理器范围。若状态为 unavailable 或 `view=current` 在 `/scope` 返回 HTTP 503 `log_scope_unavailable`，保留旧行但标记过期，停止声明当前运行并显示所有者错误；待所有者恢复后重试，不能猜成管理器会话。筛选 HTTP 400 `log_node_path_invalid` 或 `log_correlation_id_invalid` 附在对应输入路径。`/query/cursor` 的游标 400 解释后只重置被拒绝的查询。警报不回显不透明令牌或筛选值。

按 IsOS 接收序号／游标顺序显示，不能按 `timestamp` 排序服务端行，也不能把浏览器行合并入运行。若 IsOS 提供事件身份，则据此去重；否则依赖不透明游标与响应边界，不能用时间戳／消息作为去重键。最多保留 1,000 行。同一时刻仅一个请求；可见且启用 Follow 时按有界间隔轮询，隐藏或暂停的面板不轮询。请求失败时保留游标与行，退避至 30 秒。恢复时先核对状态。收到 `log_cursor_expired` 或 `log_collector_gap` 时沿服务端同运行恢复令牌继续，并保留缺口横幅直至操作员确认；`log_run_changed` 则开始新范围与边界。不能盲目重置到无范围的第一页。单独的 `rotated` 标志不再静默清空行。

面板明确区分五种非记录状态：尚无合格锚点（`pending`）、正常空页且游标推进／无诊断的筛选后无匹配、锚点不可用（`log_anchor_unavailable`）、收集器不可用、保留缺口。锚点不可用和保留缺口显示丢失警报；收集器不可用显示连接警报。新运行边界显示新旧安全 ID，清空旧详情；P07 不提供历史运行选择器。已有旧运行时，管理器会话中的包／Apply 故障仍为管理器事件，可通过旧版来源视图及状态查看；它不改派 P07 当前运行范围。Home 状态徽标及进程就绪只来自 IsOS 状态，不能从日志的 `eos_ready`、`deployment_ready` 或 PID 推断。首失败与首就绪使用同样的锚点导航，并明确标注结果。

process-control 在 `/run_id` 返回 HTTP 500 `log_scope_publish_failed` 时，先显示类型化故障，不能声称已有子进程。失败或不确定的持久边界可能留下旧范围，也可能留下幸存的新运行；使本地请求代际失效，重读状态与 `view=current`，不能假定其中一种。新范围若在无子进程时幸存，IsOS 可能返回不可用锚点；显示丢失诊断，不虚构就绪。

UI 将事件字段作为转义文本呈现，展示来源／核心／运行／关联及保存图指针，不把指针当文件路径链接，仅复制安全 ID。行的 `role=log` 使用 `aria-live=off`；独立 `role=alert` 报告收集器错误与缺口，`aria-live=polite` 只播报一次新运行或锚点。两种语言文案区分状态、缺口和筛选后空结果。刷新后键盘焦点仍在筛选器或“跳到首事件”按钮。来源／节点／关联输入受 IsOS 契约长度约束，非法输入不会发请求。

[夹具](../../ui/tests/fixtures/log_cursors/README_cn.md)冻结规约器及服务边界：管理器故障、启动切换、筛选前首就绪／首故障、旧运行响应竞态、有无丢失的轮转、过期游标、收集器故障／恢复、新运行及旧 API 回退。实现测试应使用桩 IsOS HTTP 服务和伪定时器，再构建本地和托管产物。完整输出保存在忽略的 `logs/test/<run-id>/`。

设计证据（2026-11-01）：使用 Node 22.23.2 与 npm 10.9.9，`python3 tests/fixtures/log_cursors/check_log_cursors.py` 校验 31 个唯一用例及十份中英文链接文档。JSON 语法、Python 编译和 `git diff --check` 通过。`npm test` 通过 17 个文件中的 62 项测试；`npm run build` 通过类型检查、本地／托管两种构建及清单校验。UI 源码未改动，因此这些是设计产物和现有基线检查，不代表 P07 行为验收。完整命令及输出位于忽略的 `logs/test/20261101-p07-log-cursors-design-final/`。

实现证据（2026-11-01）：服务以精确详情查询请求 `view=current`，验证归属方响应后才进入当前运行视图。面板固定未筛选的首事件，按接收顺序显示日志，轮转时保留已有行，按代码有界记录缺口，重置被拒游标，区分新运行，在收集器故障后等待 30 秒重试，并将浏览器诊断独立显示。Run 的类型化发布故障会中止并使之前的在途读取失效，把旧行标为过期，然后立即重读状态和当前日志。同一运行内已确认的缺口计数不会反复出现；新增丢失计数会再次报警。运行身份不可用有独立双语文案，不混同锚点丢失。锚点从待定变为已找到时仅礼貌播报一次。HTTP 桩及挂载面板的伪定时器测试覆盖这些竞态与状态，以及管理器故障、首条就绪／故障、轮转、游标拒绝、503、收集器恢复和旧版回退。使用 Node 22.23.2 与 npm 10.9.9，干净的 `npm ci` 通过，`npm test` 通过 20 个文件中的 83 项测试，`npm run typecheck` 通过，`npm run build` 校验本地和托管两种构建及清单。P07 夹具检查器校验 31 个唯一用例和十份双语文档。完整输出和可复现命令位于忽略的 `logs/test/20261101-p07-isosui-review-fixes/`。当前视图需要 IsOS P07 服务端支持；旧版 IsOS 仅显示兼容提示，不声称找到首事件。
