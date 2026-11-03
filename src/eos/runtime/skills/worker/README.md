# Skill worker transport and grants

This directory contains worker IPC **1.0.0** (wire encoding `1`), the first isolation boundary for out-of-process skills. The protocol is internal to one EOS build; a version mismatch fails admission rather than negotiating a legacy path.
`protocol.h` exchanges one bounded message per Unix `SOCK_SEQPACKET` packet. Each
packet carries a protocol version, type, request ID, binding generation, and at
most 64 KiB of inline payload. An optional read-only sealed memfd carries up to
64 MiB of larger data. The receiver owns and must close the received file
descriptor. The accepting side must verify `SO_PEERCRED` against the worker
process it launched before admitting a session.

The broker issues random capability tokens only after the runtime binding
manager resolves execution arguments to canonical device IDs. A token binds one
execution, generation, exact primitive and version, device ID, and expiry. The
broker must call `GrantTable::Authorize` immediately before each Dispatcher
call, and revoke grants when an execution ends or its generation retires. The
worker must never receive a Dispatcher pointer or a raw backend socket.

`eos-skill-worker` loads ABI v2 libraries in a separate process. The
`NativeWorkerProvider` proxy discovers its catalog and launches one worker per
execution; its broker checks execution-scoped grants before forwarding primitive
calls. Without a grant resolver, hardware calls fail closed. Pure native skill
execution and denied hardware calls have process-level tests.

EOS startup uses this provider for packaged ABI v2 skills. Without configured
capability slots, hardware requests fail closed. Declared resource reads, inference, source publish/query/wait, SIGNAL
receive/emit, and checkpoint calls are brokered. Child-process callbacks are
not yet bridged. Production startup registers no built-in skill definitions.

The worker applies `no_new_privs`, resource limits, Landlock, and seccomp before
loading skill code; admission fails if enforcement is unavailable. Cancellation revokes grants, relays `CancelCommand` for active primitives,
and terminates the worker; an uncertain hardware result is reported explicitly. This is the production skill entry path; the callback bridge remains
partial.

An enabled package module may set `skill_directory = "lib/island/skills"` at the
module top level. EOS resolves it inside the exact release selected by
`isos.lock`; the path may not escape that release. Enabled modules may declare
`[[skill_capability]]` entries naming an exact skill and primitive, robot ID,
device class, and `argument_pointer`. The argument at that JSON pointer is a
stable part ID (for example, `"default"`). EOS resolves it against the admitted
hardware Registry and gives the worker an opaque grant. ABI v2 skill code uses
the stable part ID as its device selector; the worker maps it to the canonical
device ID only when sending a broker call. No generated device ID appears in
package configuration or execution arguments.
