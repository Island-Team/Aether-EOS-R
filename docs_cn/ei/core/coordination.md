# 协同 profile 执行

[English](../../../docs/ei/core/coordination.md) · [核心总览](modules.md) · [决策](decision.md)

`src/egia/core/decision/coordination/` 是可选多执行器决策路径。启用时替代串行 decision entry，但仍使用同一 mission table 与 `SafetyGate`。它是 validated profile DAG 上的增量市场：能力决定可行性，成本排序设备，依赖释放工作，zone lease 仲裁空间。

## 所有权与数据

- `bidders.py` 为每个至少公布一个 primitive 的设备生成 `BidderProfile`，身份为 `backend:device_id`，可变状态为健康、承诺时长、当前 zone、上一 primitive。
- `auction.py` 输入 ready step 与 bidder 快照，输出 award、价格、bid 和未分配 step。
- `board.py` 拥有有界 job 与 `WAITING -> READY -> RUNNING -> DONE | FAILED` 状态。
- `leases.py` 只管理内存中的互斥 zone claim，不是物理锁。
- `coordinator.py` 管理 intake、tick、执行队列、再发现、结果观测和 mission 结算。

## 完整流程

```mermaid
flowchart TD
  mq["MissionQueue.wait_pending"] --> run["标记 running"]
  run --> writer["ProfileWriter：order 到 validated DAG"]
  writer --> board["有界 TaskBoard"] --> ready["依赖已完成步骤"]
  discovery["EOS discovery"] --> bidders["设备 bidders"] --> auction
  ready --> auction["epsilon-raising auction"] --> lease["可选 zone lease"]
  lease --> queue["每 bidder 一个 FIFO"] --> gate["SafetyGate.execute"]
  gate --> result["结果观测与结算"] --> ready
```

Intake 取出 pending mission、标记 running，并发构建 profile。writer 失败即失败任务； board 满则以 `coordination board full` 失败。根步骤初始 `READY`，其余等待所有 `after` 依赖变为 `DONE`。

每次 tick 向非 busy bidder 提供 ready step。bidder 在不健康、缺少精确 primitive，或 非空 station 集不含目标 station 时 abstain；空 station/tag 表示未知/不限制，EOS 当前 不提供这些信息，EGIA 不推断。原始成本由执行时长、zone 切换、primitive 切换和 congestion×承诺负载组成。auction 加 standing price，以 bidder ID 打破平局，每轮调用 每 bidder 最多一个 award；争用时按最小 runner-up gap 加 `epsilon` 涨价。64 轮后仍未 分配的步骤保留 ready，等待后续 tick。

带 zone 的 award 必须取 lease。当前 wiring 使用优先级 `1.0` 且只接受 `ACQUIRED`， 已有 holder 时延后 claimant；通用 lease 类型虽可表示抢占，本协调器不抢占运行步骤。 执行器复制参数，为非 fallback 设备补 `part_id`，构造显式目标调用并在 `step_timeout_s` 内通过 gate。成功必须同时满足 gate admitted、存在 ack、 `ack.accepted=true`。结果以 `kind=result` 进入 perception。成功释放 lease、结算负载、 完成步骤并释放依赖；最后一步完成会成功并移除 job。gate 拒绝、EOS 拒绝、缺 ack 或 超时都会失败步骤与 mission，下游不执行。

## 刷新、关闭与边界条件

强制发现创建初始 bidder；失败 backend 不贡献 bidder。间隔为零时禁用周期刷新。 新设备增加队列/执行器；返回设备保留负载/zone 历史并刷新能力；消失设备变为不健康， 不再接新 award。已排队工作不会中途杀死或重分配，刷新失败保留旧市场。

无 mission、无 ready 工作、无可用 bidder、无可行 bid、zone 被占用时都等待而不虚构 分配。未知或重复状态转换抛错。空 profile 无法满足 `BoardJob.finished()`，必须由上游 profile 校验拒绝。意外 executor 退出只记录日志，不单独重启。关闭会取消并 drain intake、tick、discovery、executor 与 mission task，绝不把取消标成成功。

任务接纳见 [Missions](missions.md)，profile 校验见 [Decision](decision.md)，最终权限见 [Safety](safety.md)。
