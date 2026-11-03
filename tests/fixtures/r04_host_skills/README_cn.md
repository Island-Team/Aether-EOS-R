# R04 技能宿主契约用例

[English](README.md) · [EOS 所有者契约](../../../docs_cn/eos/interfaces/skills/native/host-abi_cn.md) · [Rust 设计](../../../docs_cn/eos/design/runtime/skills/host-rust_cn.md) · [首次使用](../../../docs_cn/eos/develop/host-skills_cn.md)

`selection.json` 是 IsOS 所有的 `isos/skill-selection.json` 形状示例；摘要和私有路径都是占位符，并非已验证的软件包证据。`cases.json` 冻结 R04 实现与边界重放计划。测试代码必须在运行时建立真实 P01 收据与实际库摘要，再逐项注入所列故障。静态用例表不证明功能已实现。`python3 check_cases.py` 检查 ID、必需边界、负例种类和 JSON 结构。R04-21 检查 guest 可见的环境与描述符，证明私有重新执行未传入 IsOS grant、秘密或设备句柄。

端到端重放须将所选 IsOS 生成器与实际默认 Rust `aether-runtime` 配对，而不是可选 C++ worker。使用最小 ABI v2 测试库、原子计数的 `execute` handler、一个声明的 primitive 和模拟 Dispatcher adapter。数据无效用例不得创建 worker；描述符无效用例可在受约束 worker 中运行构造函数或查询函数，但 handler 计数保持零；未声明 primitive 不向 Dispatcher 发命令。记录进程 PID 和回收结果，以及共用 `run_id` 与 `execution_id`。R04 完成前将全部命令和输出保存在 `logs/test/<run-id>/`。

进程 fixture 位于 `crates/aether-runtime/tests/host_skill_catalog.rs`，编译已选择的旧前缀 C guest，覆盖成功、原语拒绝与阻塞的非重入运行。它检查真实受管 `base.velocity` 日志项和 `primitive.ack`，以及调用拒绝锁闭和排队取消。私有 Dispatcher 释放竞争测试位于 `crates/aether-dispatcher/tests/gateway_stop.rs`。合并后的 EOS/IsOS 可检查退休重放记录于[首次使用指南](../../../docs_cn/eos/develop/host-skills_cn.md#r04-精确合并版本可检查退休验收2026-11-02)。

`preflight_cases.json` 固定八项跨所有者候选检查：结构化结果、错误 ABI 或缺少就绪 binding 时保留旧服务、修订／generation 绑定、惰性进程边界、接近期限的超时和可检查的候选 grant 清理、畸形输出、启动时 TOCTOU 复核及自动子进程重启。`check_cases.py` 验证 ID、字段、边界及 IsOS 诊断代码。精确合并版本的实时重放覆盖旧运行 Stop 确认与未知、重试栅栏、保留事件及保存图的启动复核；该重放未覆盖完整实时 90 秒超时及预检后可执行文件／库变更。定向测试与较早的受控变更运行提供独立证据。R06 执行器停止／模拟器回收有独立验收门槛。
