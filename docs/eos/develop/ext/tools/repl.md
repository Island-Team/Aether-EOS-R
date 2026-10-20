# Diagnostic REPL

[Extensions and backends overview](../README.md) defines the shared data boundary. `src/eos/ext/repl` is a standalone HTTP/SSE client for `eos-north-http`; it has no gRPC stub or direct EOS connection path.

Start the processes independently:

```sh
./bin/eosd --config "$PWD/etc/eos/eos.example.toml"
./bin/eos-north-http
./bin/eos-repl
```

Or use `./eos-launch.sh`, which supervises simulator → EOS → north HTTP → REPL for development while keeping production process ownership separate.

## Options

| Option | Default | Meaning |
| --- | --- | --- |
| `--host ADDR` | `[north_http].host` | north HTTP host |
| `--port N` | `[north_http].port` (`16005`) | north HTTP/SSE port |
| `--config PATH` | `etc/eos/north-http.toml` | source for north HTTP defaults |
| `--contracts PATH` | `EOS_CONTRACTS`, then `ISLAND_SHARE` | offline/older-server fallback schemas for descriptions and advisory checks/hints |
| `--connect-timeout-ms N` | `2000` | north HTTP liveness connection deadline |
| `--timeout-ms N` | `630000` | unary request deadline; SSE reconnects use bounded internal backoff |
| `--no-history` | off | disable persistent command history |

Unknown options, missing values, malformed numbers, and non-positive timeouts are rejected before startup. The client connects when north HTTP is live, then calls `Discover`; if EOS is not ready that request reports north HTTP's `503`. On success the discovered robot name becomes the prompt identity.

## Commands

```text
discover                              catalog and robot/device health
status                                runtime status
audit-status                          audit storage health and usage
status base|arm|gripper [robot]       normalized hardware status resource
resources                             list resource URIs
read <uri>                            read one resource
history <uri> <selector> ...          query retained resource products
watch <uri>                           modal resource SSE watch; q cancels
capture <robot> <camera> [1|2]        capture bounded JPEG frames
observe-objects --json [{...}]        capture and detect objects in one request
perception-roles                      list configured perception model roles
infer-vad --json [{...}]              typed VAD request
detect-humans --json [{...}]          typed human detection request
estimate-human-pose --json [{...}]    typed human pose request
detect-faces --json [{...}]           typed face detection request
call <primitive> <ver> <device|-> ... call a primitive
cancel-motion <command-id>            cancel a primitive command
auto base [device-id]                  drive streamed base navigation with keyboard nudges

skills                                list skill descriptors
skill <name> <version>                get one descriptor
start <name> <version> ...            start and attach to an execution
executions                            list executions
execution <id>                        get one execution
state <id>                            get public execution state
watch-execution <id> [sequence]       stream execution snapshots
watch-state <id> [revision]           stream public execution state
journalctl <id> [journal-sequence]    follow the execution journal
ps [skill [version]] [--status ...]   show a process-style execution snapshot
ps [skill [version]] --watch          live execution monitor
kill <id>                             cooperatively terminate one execution
pkill <skill> [version]               terminate matching active executions
sig <signal> <id> ...                 cancel, control, or signal an execution

workflows                             list ActionLists
workflow apply [--json|--prompt]      apply a workflow definition
workflow <id>                         get one ActionList
workflow watch [id]                   watch workflows; Ctrl-C terminates a targeted workflow
workflow templates                     list registered local templates
workflow show <template>              print one registered template
workflow run <template> [id]          submit a fresh template copy
runtime                               get the manager runtime snapshot

describe primitive <name> <ver> [device]
describe skill <name> <version>
refresh                               refresh runtime completions
set full on|off                       show or elide large media fields
set brief on|off                      concise known resource and state views
connect [host] [port]
disconnect
help [command]
leave                                exit only the REPL, return to shell; keep EOS running
quit
```

When discovery contains exactly one robot, the robot argument to a hardware status command may be omitted. With multiple robots it is required. These aliases issue `ReadResource` for `robots/{robot}/base/status`, `robots/{robot}/arm/status`, or `robots/{robot}/gripper/status`; bare `status` continues to call `GetStatus`.

Resource history requires exactly one selector. Ranges are inclusive and the explicit limit must be between 1 and 1000:

```text
history robots/demo/detection/state --sequence 42
history robots/demo/detection/state --before 2026-10-04T12:00:00Z
history robots/demo/detection/state --after 2026-10-04T12:00:00Z
history robots/demo/detection/state --nearest 2026-10-04T12:00:00Z
history robots/demo/detection/state --range 20 42 --limit 10
```

The typed perception commands accept the existing RPC request as a ProtoJSON object. `observe-objects` takes `robotName`, `deviceId`, and `modelId`, with an optional `sourceTimestampNs`. Protobuf `bytes` fields such as `input.data` use base64; bare `--json` opens the multiline JSON editor.

`state ID` reads the latest skill-published public state. `watch-state ID [revision]` follows later revisions and resumes from the last processed SSE ID; this payload is distinct from execution lifecycle snapshots and journal entries. `set brief on` selects concise rendering for recognized resource and execution state views without changing the wire response or watch cursor.

`ps` shows queued and running execution records by default; use `ps --all` to include terminal work. `ps --watch` is the live monitor and exits with `q`. Execution-taking commands tab-complete full retained execution IDs. `sig` completes supported signal names first, then retained active execution IDs when the catalog can determine support, followed by schema-backed payload keys and values.

For primitive, skill, and signal commands, `key=value` is the fast path. Obvious values are converted to null, boolean, integer, number, array, or object; quote a numeric-looking value to keep it a string. A malformed array or object is rejected locally. `--json {…}` accepts a complete object, while bare `--json` opens the multiline editor: Enter submits, Ctrl-J inserts a newline, Ctrl-C cancels, and invalid JSON stays available for correction.

`auto base [device-id]` sends a navigation goal for each direction key event (default device `base-0`). W/S send forward/backward relative targets, A/D send left/right lateral targets (left is positive Y), and J/K send yaw targets; Q quits. `[` slows and `]` speeds the nudge size in 25% steps from 25% to 200% (default 100%). At 100%, the nudges are 0.24 m translation (2× the original default) and 0.45 rad yaw (3× the original default); this is a target scale, not a calibrated metres-per-second rate. The REPL sends `base.navigate` with `stream=true`; each update gets a new command ID and refreshes the latest target. The REPL sends no synthetic key-hold updates; a held key relies on terminal key-repeat events. The selected backend defines target replacement, inactivity, and cancellation behavior. Quit, interruption, hangup, or request failure ends auto mode without requesting `base.posture stand`; `motion.stop` remains available.

```text
call arm.movej 1.0.0 arm-0 reference=absolute joint_positions_rad=[0,0,0,0,0,0]
start observe_human 1.0.0 robot_name=robot-0 gimbal_device_id=gimbal-0
sig application.signal EXECUTION_ID --json {"field":"value"}
workflow apply --json {"definition":{"workflowId":"demo-1","actions":[{"actionId":"stop","primitive":{"name":"robot.stop","version":"1.0.0","deviceId":""},"parameters":{}}]}}
workflow templates
workflow show register_host
workflow run register_host debug-register-host
workflow watch debug-register-host
```

## Registered workflow templates

EOS registers valid JSON files from `<share-root>/workflows` at startup and publishes the catalog through `WorkflowService`. The REPL discovers that remote catalog when it connects, just as it discovers skills. Registered templates are not active ActionLists: `workflow templates` lists them, `workflow show <name>` prints a definition, and `workflow run <name> [workflow-id]` asks EOS to copy and submit one. Without an explicit ID, EOS adds a fresh UUID suffix to the template's `workflowId`, so repeated debugging runs do not collide.

`workflow watch` watches the global ActionList event stream and `q` detaches. `workflow watch <id>` filters the view to one workflow; `q` still only detaches, while Ctrl-C submits a no-op replacement that supersedes the target workflow. Cancellation is cooperative, so acceptance means termination was requested and the old workflow may remain `SUPERSEDING` briefly before becoming `SUPERSEDED`.

`application.signal` and its payload above are schematic: use a name and schema listed under `acceptedSignals` by `describe skill`. `sig cancel ID` maps to manager termination. `sig pause ID` and `sig resume ID` map to the declared `skill.command` signal with a `command` payload and take no additional payload.

`--prompt` provides guided entry for primitives, skill starts, signals, and workflows. It shows required fields first, offers optional fields, displays types/defaults/descriptions plus enum, boolean, and numeric range choices, checks required fields, basic types, choices, and direct numeric bounds, prints the final JSON, and asks for confirmation. Workflow submission uses `--json` or `--prompt`; the other parameterized commands also retain `key=value`. For primitives, remote per-device `parameters` from discovery drives `describe`, prompting, advisory checks/hints, and completion; a local `--contracts` schema is used only when an older server omits that metadata or while offline. `describe primitive` also displays the separate discovered device limits; `describe skill` uses the live skill descriptor. Complex root conditionals and nested array/string constraints remain EOS-authoritative and are not duplicated by the REPL's lightweight checker. An explicitly empty remote schema means the primitive accepts `{}`; it does not trigger local fallback or raw entry. `key=value` and `--json` remains the complete non-interactive escape hatch and is appropriate for scripts.

Interactive terminals provide UTF-8 editing, arrow navigation, unique history, Ctrl-R search, syntax highlighting, bracketed paste, inline hints, and context-aware Tab completion. Piped input is prompt-free. History is limited to 1,000 entries with user-only permissions at `$XDG_STATE_HOME/island/eos-repl/history`, or `$HOME/.local/state/island/eos-repl/history`; use `--no-history` for sensitive commands.

Resource, journal, and execution-monitor watches are modal. Press `q` to close only that view and return to the prompt. Streams remember the last processed SSE `id` and reconnect with `Last-Event-ID`; an explicit sequence remains authoritative. Closing a watch cancels its HTTP connection, and north HTTP cancels the upstream gRPC context. A session admits at most 16 concurrent watches and limits one buffered SSE event to 8 MiB. Timeout, unavailable north HTTP, invalid ProtoJSON, and EOS errors retain their numeric gRPC code, message, and request ID. An HTTP rejection or malformed SSE error terminates that modal watch instead of leaving it attached to a dead stream.

`start` attaches after a successful launch and reuses the execution SSE watch; it returns automatically at a terminal status. Use `--detach` to return as soon as the launch succeeds. No execution timeout is sent unless `--timeout DURATION` is present. While attached, Ctrl-C cancels the execution without terminating the REPL and immediately detaches after submitting the cancellation; `p` pauses, `r` resumes, `s` opens a signal/payload prompt, `d` or `q` detaches, and `?` shows the controls. Terminal hangup detaches, and terminal mode is restored on every exit path.

Responses use method-aware views rather than raw JSON. Large base64 media is elided unless `set full on`. REPL process logs are isolated under `var/log/eos/latest/repl/` and share request IDs with north HTTP and EOS logs without recording arguments or response payloads.

Skill execution views display the stable terminal reason and the bounded operator message carried by the snapshot. Scripts must branch on status and reason rather than parsing that message. Correlate a failure with EOS's single `skill_execution_terminal` warning; the message is sanitized and may be absent for legacy skills or cancellation.

See the [north HTTP contract](../../interfaces/external/http.md), northbound [body](../../interfaces/body.md), [world](../../interfaces/world.md), [primitives](../../interfaces/actions.md), and [skills](../../interfaces/skills.md) references.
# Navigation points

Use `waypoints <robot>` to list points, `waypoint get <robot> <name>` to read one, and `waypoint put <robot> <name> <x-m> <y-m> <yaw-rad>` to upsert one. `waypoint delete <robot> <name>` removes a point. After directly editing the private JSON snapshot, run `waypoint reload`.

Tab completion suggests the waypoint subcommands, discovered robot names, and point names observed through `ListResources`. Run `refresh` after adding, deleting, or directly editing points to refresh those name suggestions.
