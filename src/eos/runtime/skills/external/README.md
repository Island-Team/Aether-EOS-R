# External skill runtime placeholder

This directory owns the future supervised external-process profile. It is not an executable implementation and contributes only an empty CMake interface target, so an external package cannot be admitted or launched accidentally.

The implementation must keep process creation, sandbox policy, IPC framing, capability enforcement, and termination in C++. The official Python SDK will bind the reusable C++ protocol client with **pybind11**. It must not use `ctypes`, expose EOS pointers, load Python into `eosd`, or import package code during admission. Prebuilt SDK wheels and inherited invocation descriptors avoid compilation and connection discovery on the launch path.

See [skill authoring](../../../../../docs/eos/develop/interfaces/skills/native/README.md) and the [external runtime boundary](../../../../../docs/eos/design/runtime/skills/external.md).
