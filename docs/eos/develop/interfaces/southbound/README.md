# Southbound hardware interface

EOS consumes hardware through the native C++ HAL in [`eos/hardware/backend.h`](../../../../../src/eos/include/eos/hardware/backend.h). It admits drivers, reads their feeds, and releases validated commands through the dispatcher. These C++ values are not public protobuf or JSON Schema contracts.

- [Driver implementation contract](driver.md)
- [Body and actuator state](body.md)
- [Commands and primitives](primitives.md)
- [World and sensor inputs](world.md)
- [Contract ownership note](../../../../../share/contracts/eos/southbound/README.md)
