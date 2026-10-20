# 南向硬件接口

EOS 通过原生 C++ HAL [`eos/hardware/backend.h`](../../../../../src/eos/include/eos/hardware/backend.h) 使用硬件。EOS 准入驱动、读取数据流，并经 Dispatcher 释放已验证命令。这些 C++ 值不是公开 protobuf 或 JSON Schema 契约。

- [驱动实现契约](driver_cn.md)
- [本体与执行器状态](body_cn.md)
- [命令与原语](primitives_cn.md)
- [世界与传感器输入](world_cn.md)
- [契约归属说明](../../../../../share/contracts/eos/southbound/README.md)
