# Repository agent guide

Follow the repository's code and documentation conventions in
[`docs/island/conventions.v1.3.1.md`](docs/island/conventions.v1.3.1.md).

## Runtime direction

- The virtual Cargo workspace contains `crates/isos` and `crates/aether-*`. Keep source packages at workspace version `0.2.1`; `adapters/eos-cxx/` is the opt-in C++ side chain. The manager binary is `isos`; the five EOS Rust executable names are `aether-runtime`, `aether-profile`, `aether-http-bridge`, `aether-config-server`, and `aether-tui`. Public EOS commands are thin launchers for these binaries.
- Rust is the primary EOS runtime. The 0.x target retains only narrow CXX/C ABI device adapters and removes the unused legacy C++ runtime after last-caller checks; this cleanup is not part of the documentation round.
- Keep hardware adapters isolated by node. The registered node type defines placement, allowed capabilities, settings, ports, and lifecycle needs; the instance profile supplies identity and settings. The runtime owns live status and health; the dispatcher owns command admission and safety. A backend must not create a second actuator route.
- For MuJoCo v2, IsOS installs or links `island/mujoco-sim@0.1.1`, chooses the scene, writes the graph, and resolves its verified executable and scene. EOS treats that graph as read-only, supervises the simulator as its direct child, creates the declared camera/base/manipulator adapters, and connects to its owner-only Unix socket. Keep backend package metadata and node settings distinct from typed hardware payloads. EOS must stop admitted actuators before terminating MuJoCo and must not mutate the applied graph.
- Use the hierarchy `core → layer → group → node`: a core is the runtime owner, a layer is a major domain, a group is a high-level node type family, and a node is a configured instance. A registered node type determines placement, description, ports, capabilities, and lifecycle. New graphs use format `0.2.0` with only the `isos` core and `isos.<layer>.<name>@v0.2` type IDs; supported older graphs remain read-only until explicit migration. EI (formerly EGIA) and EOS are separate processes and domains, not separate graph cores. Source packages are `0.2.1`; wire interfaces and external package versions are independent.
- Navigate node design through the [EOS layer catalog](docs/eos/layers/README.md). A node page describes one registered type family and links to its authoritative contract under [interfaces](docs/eos/interfaces/README.md); deployment instance IDs belong to the IsOS graph. The 0.x target does not add a Workflow/ActionList engine or graph-authored skill sequence. Keep their published protobuf identities as historical compatibility, indexed under [legacy](docs/eos/legacy/README.md).
- The default EOS build and public launchers use Rust. The C++ runtime is currently an unlaunched, opt-in side chain. The target uses one versioned node profile for identity, capabilities, routing, and configuration; do not add a second contract manifest. Remove old code and documentation only after replacement behavior and tests are complete, retaining the active CXX mock, adapter ABI, and EGIA-required schemas.

## Development workflow

For each 0.x milestone, follow the [development-agent delivery cycle](docs/island/implementation-milestones.md#milestone-delivery-cycle-development-agents): agent planning, owner contracts and design, independent design review, implementation slices, integrated evidence, independent implementation review, and first-use replay before closure.

- Use separate `refactor(v2)/<topic>` branches for independent work and milestones, including changes in sibling repositories. Keep each branch focused and its commits reviewable by subsystem.
- Push each completed branch and open a pull request against the repository's integration branch (normally `master`). Run the relevant checks, review the PR, then squash merge it. Do not commit implementation work directly on `master`.
- After the squash merge, confirm the target branch is updated on the remote, then delete the completed topic branch both remotely and locally. Remove any worktree created for that branch after preserving needed test logs. Leave unrelated branches and worktrees alone.
- For each substantial module, add unit tests with mocks and an integration test with a stub, simulator, or ROS 2 graph as appropriate. Test failure and stop behavior as well as success.
- Run formatting, lint, focused tests, and applicable integration tests before merging. Save complete test and benchmark output in `logs/test/<run-id>/<module>.log`; summarize reproducible commands and results in the relevant development or design document. The `.log` files are local artifacts and are ignored by Git.
- Format every Rust workspace target with the root [`rustfmt.toml`](rustfmt.toml): run `cargo fmt --all` before committing and `cargo fmt --all -- --check` before merging. Keep formatting-only changes separate from behavior changes.
- Benchmarks should use fixed workloads and report throughput, median, and p99 latency as an initial baseline. Do not assert a performance target without a measured baseline and a stated environment.
- Keep English and Chinese documentation links in sync. Document every public profile or data field with its type, unit, purpose, and validation.

Start with the [hardware interface](docs/eos/interfaces/hardware.md), [hardware design](docs/eos/design/runtime/hardware.md), [dispatcher design](docs/eos/design/runtime/dispatcher.md), and [development guide](docs/eos/develop/hardware.md) when changing the Rust foundation.

For the full runtime replacement, follow the [foundation plan](docs/eos/design/runtime/foundation.md) and [0.x implementation roadmap](docs/eos/maintenance/todo.md). Use `refactor(v2)/runtime-main` as the integration branch and squash merge completed implementation slices into it. Track the last caller of each old C++ component and delete the component when its Rust replacement passes equivalent tests and no installed target needs it.
