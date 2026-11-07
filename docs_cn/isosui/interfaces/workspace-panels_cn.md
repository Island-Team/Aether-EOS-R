# U03 工作区面板契约

IsosUI 只拥有浏览器本地布局。持久化键为 `isosui:workspace:v1:<robot-id>`，内容为 `{version: 1, panels}`。`control`、`monitor`、`vision`、`logs` 各有布尔值 `open`、`collapsed`、`docked`，以及有限像素数值 `x`、`y`、`width`、`height`、`z`。无效或旧版值恢复默认布局。加载和视口改变时限制几何位置。机器人 ID 来自 IsOS 保存的图。布局不修改图字节，也不授予命令权限。

一个持久 EOS 工作台在 Graph/EOS 页面间保持控制、监控和视觉状态；一个日志消费者持有 P07 游标。停靠或收起时内容仍挂载；关闭控制面板时隐藏面板，同时在内存中保留本次运行的命令记录；关闭日志面板会卸载，重新打开时必须检查 IsOS 状态和当前范围。隐藏或收起的监控、视觉、日志面板停止轮询或流；隐藏控制面板暂停自动结果读取。所有数据仍按所有者运行和活动修订围栏。

固定工作区区域显示 IsOS 连接状态与活动修订，并始终提供 IsOS Stop。Stop 意图会同步阻止所有面板和页面中的 Invoke 与结果跟踪。只有 IsOS 的类型化 Stop 响应才能标记停止已确认；未知或失败仍按原样显示。EOS 结果与 Stop 结果分开。历史 [M4 受影响边界回放](https://github.com/Island-Team/Island-eos/blob/refactor%28v2%29/runtime-main/docs_cn/island/m4-affected-replay_cn.md)通过选定 A03 负面路径与受监管任务；该 IsOS 固定版本不支持同一次运行内替换 EGIA 子进程。后续 [Q03/Q04 证据](../q03-evidence_cn.md)使用各自独立的固定输入。

**Stop 围栏解除。** 请求未完成时围栏适用于整个工作区，不受发起页面提供的运行 ID 影响。多个重叠的 Stop 请求须等待全部所有者响应。IsOS 返回 Stop 结果后，工作区重新读取所有者状态。只有新的已知运行 ID 与记录的页面、工作区及类型化终态运行 ID 不同时，或先观察到已停止／无运行状态，才可解除围栏。身份不可用、仍是已记录的运行或 Stop 尚未完成都不能解除。若旧身份未知，也没有已停止过渡或终态运行 ID，界面会继续围栏，直到能安全确认新运行。窄屏强制堆叠面板，禁用“浮动”；原先浮动的面板仍可停靠。

**Vision 来源显示已实现：** [Vision 来源显示契约](vision-provenance_cn.md)为 Vision 面板增加运行／版本／stream／generation 围栏和实际字节摘要校验。隐藏或停止面板时清除当前证据，并阻止延迟帧重新绘制。[Q03 final-17 与 Q04 final-5 回放](../q03-evidence_cn.md)分别在各自固定输入上观察到渲染画面；final-5 在回环地址模拟托管 Origin。
