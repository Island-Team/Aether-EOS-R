# Island package manifests

<!-- docs-tree:start -->
**Location:** [Docs](../../README.md) / [Island](../README.md) / [Interfaces](README.md) / **packages**

<details>
<summary>Docs tree</summary>

- [docs/](../../README.md)
  - [island/](../README.md)
    - [interfaces/](README.md)
      - **packages.md**

</details>
<!-- docs-tree:end -->

[中文](../../../docs_cn/island/interfaces/packages_cn.md) · [Install packages](../../eos/usage/packages.md)

Island-port is Island's web and service gateway. It publishes reviewed shared skill and extension manifests through `/api/packages/v1`; its `docs/api/packages.md` defines authentication and routes. The local package client, `island-pkg`, verifies a downloaded archive against its approved size and SHA-256 digest before extracting it. Backends are private local source packages outside the Island checkout and are not accepted from the public catalog. The manifest schema is independently versioned; v1/v2 releases remain installable, but their legacy activation layout is rejected. Schema v3 defines native skills, local builds, and the external module layout.

`id` is a lowercase `namespace/name`; `version` is SemVer; `kind` is `extension` for EOS integrations, `example` for install-only onboarding, `skill` for native skill libraries, or `backend` for private bridge processes. `source_url`, `release_url`, `description`, and `license` identify a reviewed public project. A downloadable asset names an HTTPS archive, SHA-256 digest, byte size, `os`, `arch`, minimum glibc, `eos_contract`, required shared-library SONAMEs, and tested Ubuntu versions. A shared native skill uses schema v3 and `eos_contract = "eos.skill.v2"`; its archive must contain at least one regular `*.skill.so`. An `example` uses an empty `eos_contract` and has no activation. The installer selects only a compatible target. There is no dependency solver or install hook.

For an EOS integration, schema v3 `activation.files` maps package-relative templates to files under `etc/ext/<namespace>--<name>/`. One destination must be `module.toml`, with `version = 1`, `[module]` identity and release fields, and optional `[[service]]` and `[[binding]]` declarations. Its `id`, `package`, and `version` must match the installed release. `{{package_root}}` expands to the installed tree, and `{{package_file:relative/path}}` expands to an installed regular file. Activation generates an isolated external configuration directory; it does not select the release, enable the module, or start processes. Add the module path to `etc/runtime/stack.toml` explicitly after reviewing it.

`island-pkg lock add NAME [--version VERSION]` selects an exact installed release in user-owned `isos.lock`; `lock remove NAME` deselects it, and `lock verify` checks the installed file tree against its pinned digest. Lock entries record package ID, version, kind, source, tree SHA-256, target, and contract. The file is sorted by package ID and written atomically. Installing or building never updates it. A selected release cannot be removed. Downloading a shared library from a direct internet URL as a package source is reserved for a future release; the lock and verification contract apply before any such source is supported.

Archive entries must be regular files or directories below the archive root; symlinks, traversal, special files, and oversized content are rejected. Package releases are immutable once approved. The center may change asset hosting later without changing client commands or the release identity.

## Private backend source packages

`island-pkg build SOURCE` reads `SOURCE/island-package.toml` and builds a CMake project into a versioned user-local installation. `SOURCE` must be outside the Island checkout for a backend. Use `--cmake-source DIR` if the CMake project differs from the private manifest directory; `target` and `install_component` limit the build and install to the bridge artifact. For example, create `/path/to/private-ros2-backend/island-package.toml` with:

```toml
schema_version = 3
id = "private/ros2-bridge"
kind = "backend"
version = "0.1.0"

[source_build]
system = "cmake"
target = "island_pkg_ros2_backend"
install_component = "island_pkg_ros2_backend"
resources = ["config/module.toml"]

[source_build.cmake_options]
EOS_BACKEND_ROS2 = true

[[activation.files]]
source = "config/module.toml"
destination = "module.toml"
```

Its private `config/module.toml` can declare the bridge, related driver units, and the binding to EOS:

```toml
version = 1

[module]
id = "private--ros2-bridge"
enabled = true
package = "private/ros2-bridge"
version = "0.1.0"

[[service]]
name = "ros2-bridge"
role = "managed"
command = [{{package_file:bin/eos-ros2-bridge}}, "--binding", "{ros2_binding}", "--profile", "{ros2_profile}", "--socket", "{ros2_bridge_socket}"]
ready_unix = "{ros2_bridge_socket}"
timeout_ms = 30000

[[binding]]
backend = "backends/ros2.toml"
profile = "ros2"
requires = ["ros2-bridge"]
eos_after = ["ros2-bridge"]
```

Add each external driver as a `role = "driver"` service and include it in the bridge's `after` list. After sourcing ROS 2, run `./bin/island-pkg build /path/to/private-ros2-backend --cmake-source .` from the Island checkout. `resources` copies regular source files into the installation after CMake install. The launch manager owns declared processes; EOS accesses the backend through its adapter endpoint. Local skill source packages may use the same build mechanism and omit activation. Skill installation does not copy a library into a loader directory or run it; the runtime manager admits selected skill workers. CMake source and its dependencies run as the calling user, so review them before building.
