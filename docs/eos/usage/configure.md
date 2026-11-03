# Configure EOS: local files and TOML

<!-- docs-tree:start -->
**Location:** [Docs](../../README.md) / [EOS](../README.md) / [Usage](README.md) / **configure**

<details>
<summary>Docs tree</summary>

- [docs/](../../README.md)
  - [eos/](../README.md)
    - [usage/](README.md)
      - [arm64.md](arm64.md)
      - [backends.md](backends.md)
      - [configuration-reference.md](configuration-reference.md)
      - **configure.md**
      - [mujoco.md](mujoco.md)
      - [packages.md](packages.md)
      - [repl.md](repl.md)

</details>
<!-- docs-tree:end -->

[Use EOS](README.md) · [Configuration reference](configuration-reference.md)

This page covers the opt-in C++ `eos.toml` configuration and its native skills and workflows. The default Rust v2 path uses the [node graph and instance profile](../interfaces/hardware.md), applied through IsOS; independent model services have separate IsOS service files. For the C++ side chain, the repository tracks templates and ignores active files. Keep device addresses, calibration, endpoints, and site routes in local copies.

## Select the external MuJoCo backend

From the repository root:

```sh
cp etc/eos/eos.toml.example etc/eos/eos.toml
```

Configure the copied process file with a binding/profile provided by the installed MuJoCo package, then select and start the package through IsOS. The build script reads the ignored `etc/eos/eos.toml` when an explicit config is supplied.

## Describe a live binding

```sh
cp etc/ext/live/binding.toml.example etc/ext/live/binding.toml
cp etc/eos/eos.toml.example etc/eos/eos.toml
```

The backend example defines one host speaker using `aplay` and ALSA `default`. Before launching, set a unique `robot_id` and `robot_name`, verify the speaker route, and add only devices and primitive limits that the deployed hardware supports. `eos.toml` selects the `live` profile from `../ext/live/binding.toml`; configure model services locally if you add model-backed primitives. The example is a starting shape, not a hardware qualification.

## Add a local built-in skill

On a clean checkout, create the ignored native skill directory from the tracked scaffold:

```sh
cp -a examples/skills/local-native share/eos/skills/native
```

If `share/eos/skills/native/` already exists, preserve it and merge the scaffold's `CMakeLists.txt`, `skills.h`, and `skills.cpp` by hand. The example registers `example.hello@1.0.0`, which returns `{"ok":true}` without accessing hardware. Add new source files to the local `CMakeLists.txt`, define their schemas and capability declarations, and call each registration function from `RegisterSkills`. EOS links `island_native_skills` only when this local CMake target exists. Reconfigure and rebuild after changing native source. For a separately loaded public ABI module, use the [native skill authoring guide](../interfaces/skills/native/README.md).

## Add a workflow template

```sh
cp examples/workflows/hello.json.example share/eos/workflows/hello.json
```

The example invokes `example.hello@1.0.0`, so install the local skill first. EOS loads every `share/eos/workflows/*.json` at startup and rejects malformed templates. Change the workflow ID, action IDs, skill or capability references, arguments, and dependencies for your deployment; select canonical hardware names from capability discovery. Historical primitive targets remain readable for saved templates. After startup, inspect it with `workflow show hello` and run it with `workflow run hello` in the REPL.

## Confirm the local files stay out of Git

```sh
git check-ignore -v etc/eos/eos.toml etc/ext/live/binding.toml share/eos/skills/native/skills.cpp share/eos/workflows/hello.json
```

Each path should print its `.gitignore` rule. The `.example` files and the skill scaffold are tracked so a fresh checkout has the instructions and shapes needed to write new local files.

## Select the process and binding files

EOS uses a process TOML to select services and one hardware binding. The commands above create ignored local copies of the tracked examples; keep site values in those copies or a separate configuration root.

The configuration root is selected by `--etc` or `EOS_ETC`. `--config` selects the process TOML. The north HTTP process reads `etc/ext/north-http/config.toml` separately. Keep absolute deployment roots in production. See [configuration](configuration-reference.md#layout) for path resolution and validation.

## Select the profile

The process file must contain one binding:

```toml
[mode]
[[mode.binding]]
backend = "backends/mujoco.toml"
profile = "benchmark"
```

`backend` is relative to the etc root. The named profile must exist in that file. A profile supplies stable robot identity, device descriptions, `[[device.primitive]]` declarations and limits, backend settings, and feed-rate envelopes. [Binding files](configuration-reference.md#binding-files) documents the schema. Never configure generated device IDs.

## Configure the other processes

| File or table | Purpose | Reference |
| --- | --- | --- |
| `eos.toml` `[gateway]` | EOS gRPC TCP/optional Unix socket | [Configuration](configuration-reference.md) |
| `eos.toml` `[log]` | stderr, text and JSONL levels, rotation, attachments | [Logs](../debug/README.md) |
| `etc/runtime/stack.toml` and external `module.toml` `[[service]]` | Managed services and persistent driver units | [Launch logic](../debug/launch.md) |
| `eos.toml` `[[perception.model]]` | Outbound model clients; model servers run separately | [Configuration](configuration-reference.md) |
| `etc/ext/north-http/config.toml` `[north_http]` | REST/SSE listener and EOS upstream | [North HTTP config](../develop/ext/north_http/config.md) |
| `etc/ext/north-http/config.toml` `[action_list_audit]` | External, non-authoritative audit projection | [Audit](../develop/ext/north_http/audit.md) |

## Validate the result

Run EOS with the selected files and inspect the startup error if strict parsing rejects a key or binding. Start north HTTP only after EOS is ready. Then query `/health/ready` and `/v1/eos/discover` as shown in the [debugging runbook](../debug/README.md#start-with-liveness-and-readiness). Changing a TOML file requires a process restart; do not assume a running process reloads it.
