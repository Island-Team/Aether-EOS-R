# Managed skill runtime placeholder

This directory is an obsolete placeholder for a former `.eos.py` direction. It is deliberately non-executable and is not included by `src/eos/runtime/skills/CMakeLists.txt`. Its local CMake file defines an interface target only if added explicitly; no managed target, parser, interpreter, or executor is built or linked into `eosd`.

Managed-looking input has no discovery, admission, or fallback execution path. Built-in C++ is the supported in-process authoring direction.

See [skill authoring](../../../../../docs/eos/develop/interfaces/skills/native/README.md) and the [managed runtime boundary](../../../../../docs/eos/design/runtime/skills/managed.md).
