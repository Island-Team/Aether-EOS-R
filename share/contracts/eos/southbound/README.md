# Southbound hardware boundary

The authoritative EOS hardware interface is [`eos/hardware/backend.h`](../../../../src/eos/include/eos/hardware/backend.h). Backends implement this native C++ HAL and are admitted by EOS before the dispatcher releases validated commands. This directory documents ownership; it defines no protobuf or JSON Schema source and has no generated wire package.

See the [hardware interface guide](../../../../docs/eos/develop/interfaces/southbound/README.md).
