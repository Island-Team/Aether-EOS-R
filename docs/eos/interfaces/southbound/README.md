# Southbound hardware interface

<!-- docs-tree:start -->
**Location:** [Docs](../../../README.md) / [EOS](../../README.md) / [Interfaces](../README.md) / [Southbound](README.md)

<details>
<summary>Docs tree</summary>

- [docs/](../../../README.md)
  - [eos/](../../README.md)
    - [interfaces/](../README.md)
      - [southbound/](README.md)
        - [body.md](body.md)
        - [driver.md](driver.md)
        - [primitives.md](primitives.md)
        - [world.md](world.md)

</details>
<!-- docs-tree:end -->

EOS consumes hardware through the native C++ HAL in [`eos/hardware/backend.h`](../../../../src/eos/include/eos/hardware/backend.h). It admits drivers, reads their feeds, and releases validated commands through the dispatcher. These C++ values are not public protobuf or JSON Schema contracts.

- [Driver implementation contract](driver.md)
- [Body and actuator state](body.md)
- [Commands and primitives](primitives.md)
- [World and sensor inputs](world.md)
- [Contract ownership note](../../../../share/eos/contracts/eos/southbound/README.md)
