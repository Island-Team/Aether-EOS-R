# EOS Rust 运行时设计

[English](../../../../docs/eos/design/runtime/README.md) · [EOS 设计](../README_cn.md) · [节点目录](../../layers/README_cn.md)

从[基础设计](foundation_cn.md)追踪进程归属与生命周期。[硬件](hardware_cn.md)描述每设备唯一适配器；[Dispatcher](dispatcher_cn.md)负责执行器准入；[进程监管](process_cn.md)负责 MuJoCo 子进程；[R06 模拟器停止](simulator-stop_cn.md)规定目标物理栅栏及终端证明。[连接调度](scheduler_cn.md)定义优先级、频率上限、有界队列和溢出处理。[读取器预检](reader-features_cn.md)说明 IsOS 启动图之前如何检查所选程序的特性。[Rust 事件](events_cn.md)把生命周期与故障映射到共享 JSONL 生产者。各节点类型有对应的[层/组/节点页面](../../layers/README_cn.md)，精确字段和示例仍放在[接口](../../interfaces/README_cn.md)。

其余 C++ 运行时模块、Workflow/ActionList 和历史模型服务在[历史目录](../../legacy/README_cn.md)中索引，不属于 0.x 目标进程计划。[实现里程碑](../../maintenance/todo_cn.md)指出尚处于设计阶段的 Rust 和 EGIA 行为。
