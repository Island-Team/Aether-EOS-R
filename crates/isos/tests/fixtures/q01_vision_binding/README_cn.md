# IsOS Vision 绑定 Q01 用例

这些固定用例属于**目标设计，不是运行时证据**。`cases.json` 固定首次实现测试使用的正反所有者预期。执行 `python3 tests/fixtures/q01_vision_binding/check_cases.py` 校验用例结构。选定 Q01 回放之后仍须测试现场路由、身份、限速与队列计数；此 fixture 本身不能关闭 Q01。

[English](README.md)

IsOS 归属方实现已在 `src/runtime/vision_binding.rs` 与 `src/runtime/control_grants.rs` 测试中覆盖选定、禁用、错误来源／门、缺失依赖、过时证明、私有授权模式、验证文件篡改及 Stop 撤销。`cases.json` 仍是设计矩阵，不是 EOS／EGIA 所选现场收据。跨归属方 Q01 回放和剩余反例仍由归属契约跟踪。

短端点用例覆盖较长的真实私有 home、两个 107 字节 socket 路径预检、按运行轮换端点、验证文件篡改、先于子进程停止的撤销、回收后的清理，以及阻止替换的孤儿目录。端点目录位于私有运行状态 `run/v/`；授权文件仍位于 `run/control-grants/`。
