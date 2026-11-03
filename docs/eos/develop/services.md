# Rust v2 service spine

The default EOS build and public launchers use this small v2 process set. `aether-runtime` owns the
dispatcher and HAL adapters. `aether-http-bridge` proxies bounded control JSON
over an owner-only local Unix socket; it has no direct adapter access.
`aether-tui` is a REPL over the same socket. `aether-config-server` serves one
validated, read-only graph snapshot; IsOS applies a changed graph by restarting
its owned processes. The bridge and config server bind only to loopback.

| Process | Required options | Endpoint or behavior |
| --- | --- | --- |
| `aether-profile` | `validate --file <graph>` | Validates TOML or JSON before IsOS admits a process graph; exit 0 means valid. |
| `aether-runtime` | `--profile <graph> --journal-dir <dir> --socket <path>` | Bounded JSON-line control socket. Owns HAL, dispatcher, durable command outcomes, and stop on shutdown. |
| `aether-http-bridge` | `--socket <path> --listen <loopback:port>` | `GET /api/eos/v2/health`, `POST /api/eos/v2/control`. The POST body is a control request with `op`. |
| `aether-config-server` | `--profile <graph> --listen <loopback:port>` | `GET /api/eos/v2/config` returns the validated source snapshot; `GET /api/eos/v2/config/validate` returns its version, robot ID, and node count; `GET /api/eos/v2/types` returns the registered node type catalog. No write route. |
| `aether-tui` | `--socket <path>` | `time`, `discover`, `health`, `capture`, `observe`, `stop`, `result`, `cancel`, and JSON `dispatch` commands. |

`aether-control` is the shared bounded control client and minimal loopback HTTP
reader. Requests are capped at 64 KiB and responses at 16 MiB; socket and HTTP
reads use two-second deadlines. `capture` returns RGB8 as `rgb8_base64` and
optional depth as `depth_u16_le_base64`; `observe` returns typed base or arm
data. The response bound covers the configured 8 MiB camera sample limit.
This small dependency graph avoids adding a
web framework to the HAL build. These processes do not load storage, skills,
or workflow modules.

Run `cargo fmt --all -- --check`, `cargo clippy --workspace --all-targets
-- -D warnings`, and `cargo test --workspace`. For the simulator path, start
`eos-mujoco-sim` with `--v2-socket`, run `aether-runtime` with the MuJoCo graph, then
exercise discovery, observation, movement, and stop through the dispatcher and
HTTP bridge. Save complete command output under `logs/test/<run-id>/`.

On Linux 7.0 x86_64 with an Intel i5-12490F, the 2026-10-29 run in
`logs/test/20261029T181500Z/` passed workspace tests, Clippy, the live HAL
test, and HTTP capture/observation/command/stop against MuJoCo. The HTTP run
returned a 921,600-byte RGB frame and six arm joints. The fixed dispatcher
benchmark measured 448,801 in-memory dispatches/s (median 1.88 µs, p99
4.18 µs); durable dispatch measured 126/s (median 7.70 ms, p99 10.34 ms).
These are baselines for this machine, not acceptance thresholds.

The 2026-10-30 full-process run used the combined MuJoCo/IsOS graph with
`ready_unix`, then `isos v2 apply`, `isos v2 run`, loopback HTTP control,
`isos v2 status`, and `isos v2 stop`. It passed catalog/discovery, RGB capture
with optional depth absent, base and six-joint observations, both commands,
safe stop, and unknown-node rejection. A forced EOS exit caused IsOS to restart
EOS and its bridge/config dependents; capture, base command, and stop passed
again, and `isos v2 stop` left zero children. The run is recorded in
`logs/test/20261029T183000Z/isos-e2e.log`. EOS separately tests recovery of
the Unix socket pathname left by a killed process.

On 2026-10-30, a focused EOS link check validated the IsOS MuJoCo graph template and ran the real `eos-mujoco-sim` executable headless with private Unix sockets, `aether-runtime`, and `aether-http-bridge` on an ephemeral loopback port. HTTP discovery returned camera, base, and manipulator nodes; capture returned 921,600 RGB8 bytes; both observations, both movement requests, both stops, and clean runtime shutdown passed. The simulator acknowledged each movement, while the dispatcher returned `OutcomeUnknown` because a transport acknowledgment does not prove physical completion. The simulator emitted a nonfatal OpenGL `0x502` warning during camera setup; the RGB frame passed decoding and dimension checks. Complete output is in the local `logs/test/20261030-eos-link/process-http-real.log`; the local runner and exact binaries are recorded beside it. The HTTP bridge also has a stubbed integration test for forwarding and 400, 404, and 502 responses, run with `cargo test -p aether-http-bridge --test bridge`.
