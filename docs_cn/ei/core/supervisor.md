# 受监督 task 生命周期

[English](../../../docs/ei/core/supervisor.md) · [核心总览](modules.md)

`egia.core.supervisor` 是小型 asyncio task supervisor，拥有 task handle、ready event 和健康投影，领域数据仍归各子系统。当前实现只有一个共享 stop event 和统一、无上限的 重启策略，不表达依赖顺序或每子系统重启上限。

## 注册与状态

`SubsystemSpec` 包含唯一名称和接收 `(ready, stop)` 的 async entry。entry 初始化后设置 `ready`，通常运行到 `stop`。`_Runtime` 保存 task、新 ready event、状态、首次 ready 时间、累计 restart/failure 与 last error。输入中的重复名称会在内部映射中折叠，因此 组合层必须保证唯一。

## 启动与就绪

`start()` 按注册顺序生成所有 entry，再启动 watchdog，但不会逐个等待。`_spawn()` 替换 ready event、清空 `ready_at`、标记 `STARTING`，task 名为 `egia-<name>`。 `wait_ready(timeout=30)` 并发等待当前所有 event；无 spec 立即成功。超时会记录确切缺失 名称并返回 false，但不取消 task。全部 ready 后补记时间、标为 `READY`。

task 可能 set ready 后在 watchdog 下次采样前退出；readiness 可短暂成功，但 `is_healthy()` 还要求所有 task 存活。健康快照保持注册顺序，首次 ready 前 uptime 为零， 负值截为零，并包含 restart 次数和最后异常表示。

## 失败与重启

Watchdog 每 0.5 秒采样。完成或取消的子系统标为 `DEGRADED`；异常记录 `repr(exc)`， 正常返回或取消记作 `task exited without error`。backoff 依次为 0.5、1、2、5、10 秒， 之后封顶 10 秒；若全局 stop 未设置，restart 计数加一并用新 event/task `_spawn()`。 failure 计数和 last error 保留历史。

Watchdog 串行处理 restart，一个 task 的 backoff 会延迟发现其他已结束 task。没有最大 restart、依赖级联、jitter、状态 checkpoint 或可选失败分类；领域 entry 必须自行保证 初始化原子性并恢复持久状态。

## 停止与边界条件

`stop()` 将全部 runtime 标为 `STOPPING`，设置共享 event，取消 watchdog，并以 `return_exceptions=true` gather 子系统。内部没有 graceful timeout，也不会主动取消 entry， 所以不响应 stop 的 entry 会阻塞关闭；gather 异常被消费。backoff 中的 watchdog 会在 respawn 前观察 stop。

重复 `start()`/`stop()` 没有显式生命周期保护，组合层各调用一次。supervisor 本身不 强制 provider-before-consumer 启动或 dependent-before-provider 关闭；这些性质来自组合 方式、entry 的 readiness 协议、共享 stop 和 `core.launch` 的显式资源关闭。
