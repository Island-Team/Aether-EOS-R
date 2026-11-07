# Terminal operator console

`libexec/egia_ext` provides the checkout's terminal operator experience. It launches EGIA as a child, consumes northbound WebSocket streams, and renders status, missions, context, prompts, model activity, primitives, native skills, and logs. It is a client, not an alternate runtime path.

## Data sources

- `/health` and `/backend` drive lifecycle status;
- `/missions/stream` drives the mission tree;
- `/missions/command` accepts short-lived mission create, update, priority, and cancel requests;
- `/context`, `/prompts`, and `/model_io` expose model-facing state;
- `/primitives` exposes primitive targets; `/skills`, `/skills/command`, and `/skills/execution` provide the executable native-skill catalog, commands, and ordered execution watches;
- perception and part routes expose current attributed observations.

Unavailable sources render as unavailable. The console does not fabricate hardware, skills, perception, or result state.

The log pane follows the active Island session replay when the console is part of a supervised launch. That ordered stream includes Island lifecycle events and local EOS, EGIA, and TUI events; source timestamps remain visible while collector sequence defines display ingestion order. Rotation is followed without discarding the pane, and attach mode switches when the guarded active-session metadata is replaced. Malformed records are ignored. When EGIA runs outside Island, the pane falls back to `var/lib/egia/egia.jsonl` and keeps the launch-boundary behavior that hides earlier runs. Structured child stderr already delivered through the Island collector is not displayed twice; unstructured traceback output remains visible.

## Navigation

The focused pane fills the console while the remaining panes stay visible as labelled dividers. Select a pane directly with `Alt+1` context, `Alt+2` missions, `Alt+3` primitives, `Alt+4` skills, `Alt+5` model stream, `Alt+6` logs, or `Alt+7` status. The skills pane shows parameter schemas, result types, and every execution observed by this console. Terminals that encode Alt as Escape plus a digit and terminals that send 8-bit Meta characters are both supported. PageUp/PageDown scroll the focused pane by a page; `F7`/`F8` provide the same actions for terminals that intercept or misreport PageUp/PageDown.

With an empty prompt, Up/Down scroll one line, Home/End jump to the oldest/newest content, and Left/Right browse primitive pages or the wide context pane's primitive-guidance column. Once text is present in the prompt, arrows, Home/End, and Left/Right edit that command instead. `help` shows the complete mapping.

## Command line

The `> ` prompt is a shell-style session command line. Known command names are cyan and unknown names red when the terminal supports color; inline hints and completion candidates are gray. Tab completes command names, mission and skill subcommands, visible mission ID prefixes, tracked execution IDs, priority bands, pane names, backend aliases, explicit primitive targets, and exact skill names and versions. Primitive and skill argument completion reads the live JSON Schema, placing required fields first and completing constants, enums, and booleans. Multiple candidates remain visible as a hint until more input disambiguates them.

Enter runs a command. Up/Down navigate history after typing; Ctrl-P and Ctrl-N provide that history navigation from an empty prompt. Ctrl-R reverse-searches session history. Standard editing keys are supported: Left/Right, Home/End, Ctrl-A/E, Ctrl-B/F, Backspace/Delete, Ctrl-U (clear line), and Ctrl-W (delete word). Command history is session-only and is never written to disk because operator calls can contain sensitive arguments.

## Commands

`chat "sentence"` sends hearing input. `call <primitive>@<robot>/<device> key=value...` sends an explicitly targeted request to `/primitive/invoke`; the existing `call <target> {"key":"value"}` JSON-object form remains available for nested or type-sensitive payloads. Shell quoting preserves spaces, as in `text="hello world"`; JSON-looking values such as numbers, booleans, arrays, and objects retain their JSON types. A missing target is shown as an input error rather than guessed. Type or complete the target, then press Tab again to insert its required parameters; `call <target>` without arguments still opens the full field guide. Planner actions and operator calls share the same safety path.

`skill start <name>@<version> robot=<alias> timeout=<seconds> key=value...` starts an exact native skill. The console generates a fresh UUID request ID, requires an explicit configured robot alias, and accepts timeouts greater than zero and no greater than 3600 seconds. Arguments use the same shell-safe typed `key=value` form as `call`; Tab completes them from the selected skill's parameter schema.

`skill show <execution-id> robot=<alias>` reads one complete snapshot, `skill watch <execution-id> robot=<alias>` follows ordered snapshots, and `skill cancel <execution-id> robot=<alias>` requests cooperative cancellation. The skills pane displays request and execution IDs, status, sequence, cancellation state, reason or message, and terminal result. A watch reconnects after transport loss using the last displayed sequence and ends at a terminal status. Command errors are reported in the log pane without inventing execution state.

`mission` manages the current mission table:

- `mission add "order"` creates an operator mission;
- `mission list` focuses the mission pane;
- `mission show <unique-id-prefix>` opens the current mission detail, including its full ID, priority band, state, and document revision;
- `mission edit <unique-id-prefix> "order"` replaces a pending mission order;
- `mission priority <unique-id-prefix> low|normal|high|urgent` changes its priority band;
- `mission cancel <unique-id-prefix>` cancels a pending mission.

Mission subcommands alone use shell-style tokenization, so quoted orders retain spaces without changing the established parsing of other top-level commands. A full mission ID or a locally unique prefix is accepted. The console resolves that prefix only against its current `/missions/stream` snapshot, rejects ambiguous or missing prefixes locally, and sends the full ID plus the current `document.revision` for compare-and-swap mutations. Each mutation uses one short-lived `/missions/command` WebSocket connection, logs the typed acknowledgement or rejection, and waits for `/missions/stream` to provide the authoritative refreshed state. Cancelled missions remain visible with a distinct cancelled state.

`repl [backend-alias]` temporarily leaves the full-screen console and runs the external EOS diagnostic client on the same terminal for current EOS resource and primitive diagnostics. Skill operation belongs to the northbound `skill` commands above and is not assumed to exist in EOS REPL. The selected backend must be reported as `up` by `/backend`; with multiple configured backends the alias is required. The endpoint comes from that live backend projection, while the `eos-repl` wrapper is resolved beside the EOS launcher selected by `island.toml`. If the backend, endpoint, or wrapper is unavailable, the console remains active and reports the error.

The EGIA process and all console feeds continue running while EOS REPL owns the terminal. Enter `quit` or `exit`, or press Ctrl-D, in EOS REPL to return to the same TUI state. A nonzero REPL exit is reported in the TUI log after the terminal is restored and fully repainted.

## Shutdown

Interactive exit from the TUI asks the child to stop and performs a bounded wait. When the TUI is running inside the composed `bin/island` launch, its clean exit also tells the product launcher to stop and reap the EOS process that launcher owns. Exiting EOS REPL only returns to the TUI. Installed services should be managed by their owning service manager; the console does not directly own EOS or model-server processes.
