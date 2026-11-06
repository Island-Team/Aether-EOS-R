# Q01 Vision 重复代际回放设计

[English](../../../docs/ei/design/q01-vision-stale-test.md) · [所有者接口](../interfaces/vision-evidence.md) · [首次使用](../develop/build-and-run.md)

Q01 需要证明选定的 EGIA 运行中子进程会拒绝当前 EOS gate stream 的重复代际。旧订阅游标不能证明这个分支，因为 EOS 仍可在同一 stream 合法地交付更新帧。私有测试入口只把 EGIA 已接受的最新帧送回现有 Vision 准入主体；它不能选择新帧，也不能进入阶段、模型、Mission 或执行器路径。

测试套接字仅在受管运行、修订、所有者授权和检查验证器均通过后，且管理器显式继承测试标记时创建。公共检查 WebSocket 保持只读。EGIA 只在测试模式中多保留一份已接受元数据，并复用现有的单份瞬时 JPEG。同一准入锁串行化普通帧接受、stream 证明、断连、停止及回放前后快照。因此，后来的合法帧若先获得锁，测试请求返回 `target_changed`；否则它须等待重复拒绝完成。

无效或目标已变化的请求不消耗机会。匹配的回放在普通准入逻辑只增加 `generation_stale` 后消耗机会，即使调用者没有收到响应，监听器也会被移除。断连或新 stream 清除回放材料；授权失效和停止还移除测试监听器。选定 Q01 进程回放及独立实现审查仍是相机步骤 4a 关闭前的必要条件。套接字和线协议的精确字段仅在[所有者接口](../interfaces/vision-evidence.md)定义。
