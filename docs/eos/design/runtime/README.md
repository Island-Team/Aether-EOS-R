# EOS Rust runtime design

[中文](../../../../docs_cn/eos/design/runtime/README_cn.md) · [EOS design](../README.md) · [Node catalog](../../layers/README.md)

Use the [foundation](foundation.md) to trace process ownership and lifecycle. [Hardware](hardware.md) describes one adapter per device; [Dispatcher](dispatcher.md) owns actuator admission; [process supervision](process.md) owns the MuJoCo child, and [R06 simulator stop](simulator-stop.md) specifies the target physical fence and terminal proof. [Connection scheduling](scheduler.md) defines priority, frequency caps, bounded queues, and overflow behavior. [Reader preflight](reader-features.md) describes selected binary feature checks before IsOS launches the graph. [Rust events](events.md) maps lifecycle and failures to the shared JSONL producer. Each node kind has a focused [layer/group/node page](../../layers/README.md), while exact fields and examples stay in [interfaces](../../interfaces/README.md).

The remaining C++ runtime modules, Workflow/ActionList, and historical model services are indexed in [legacy](../../legacy/README.md). They are not the 0.x target process plan. The [implementation milestones](../../maintenance/todo.md) identify which Rust and EGIA behavior is still design-only.
