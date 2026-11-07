# 运行和停止部署

本流程供已保存图并验证精确软件包版本的操作人员使用。**Run** 启动与保存修订绑定的候选运行；**运行 ID** 标识其生命周期。候选运行成为活动状态前，归属方必须报告就绪。

先验证草稿，依据当前修订 Apply，然后 Run。准入后重新检查状态和日志。通过[运行 API](../develop/http/runtime_cn.md)或 CLI 请求 Stop；IsOS 等待所选归属方的终止证明，并等待自己直接启动的子进程退出。物理停止结果不确定时状态保持 `unknown`，阻止安全替换。重试前按[故障排查](../debug/troubleshooting_cn.md)处理。

[归属方停止契约](../interfaces/cores/dependent-stop_cn.md)定义当前证明字段。IsOS 不会把超时或归属方断开推断为停止成功。

[English](../../../docs/isos/usage/run.md)
