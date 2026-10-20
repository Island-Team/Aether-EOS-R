# Built-in skill runtime

This directory implements the preferred trusted in-process provider. Product code explicitly registers `Definition` values containing copied metadata and a `Task<Outcome>` coroutine handler; there is no static registration or package loader.

`Registry` validates schemas and declarations, enforces exact identity and non-reentrant serialization, binds declared primitive and resource capabilities, and drains active coroutine and primitive work during destruction. `Execution` currently wraps primitive start/wait/cancel, resource reads, SIGNALs, process spawn/mailbox operations, checkpoints, and the product attachment hook. Metadata can declare timestamped sources, but built-in `Execution` does not yet expose source publish/query/wait wrappers.

Built-in code is trusted and shares the `eosd` address space. It must keep work bounded and cooperate with cancellation; primitive cleanup requests cancellation and waits for terminal drain before execution completion.

See [`docs/eos/runtime/skills/authoring.md`](../../../../../docs/eos/design/runtime/skills/authoring.md) for the author boundary.
