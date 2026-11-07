# Manage Island extensions with island-pkg

<!-- docs-tree:start -->
**Location:** [Docs](../../README.md) / [EOS](../README.md) / [Usage](README.md) / **packages**

<details>
<summary>Docs tree</summary>

- [docs/](../../README.md)
  - [eos/](../README.md)
    - [usage/](README.md)
      - [arm64.md](arm64.md)
      - [backends.md](backends.md)
      - [configuration-reference.md](configuration-reference.md)
      - [configure.md](configure.md)
      - [mujoco.md](mujoco.md)
      - **packages.md**
      - [repl.md](repl.md)

</details>
<!-- docs-tree:end -->

[中文](../../../docs_cn/eos/usage/packages_cn.md) · [Package manifest](../../island/interfaces/packages.md) · [MuJoCo setup](mujoco.md)

For a new IsOS deployment, `isos packages` is the package interface and the deployment graph selects installed versions. The [IsOS package guide](../../isos/usage/packages.md) documents search, install, verify, and removal. Python components default to Python 3.12; service launch commands may select uv, Conda, or an explicit interpreter without binding to the system Python.

The rest of this page describes the EOS checkout's compatibility package store. When IsOS is available, `island-pkg` forwards `search`, `info`, `install`, and `list` to it; `island-pkg list` and `island-pkg --list` are equivalent. Legacy `--root`/`--registry` overrides and older commands remain local to this checkout. Set `ISLAND_PKG_LEGACY=1` to use the legacy store explicitly.

## Legacy EOS package store

```sh
./bin/island-pkg search NAME
./bin/island-pkg doctor NAME
./bin/island-pkg install NAME
./bin/island-pkg lock add NAME --version 1.2.3
./bin/island-pkg lock verify
```

Installation does not enable a module, load a skill library, or start a process. `isos.lock` in the package root records each selected package's exact ID, version, kind, source, target, contract, and installed-tree SHA-256 digest. `lock add NAME [--version VERSION]` selects an installed version; `lock remove NAME` deselects it; `lock show` prints the file; `lock verify` checks every selected release against its installed files. The file is sorted and written atomically. A selected release cannot be removed. Direct installation of an internet-hosted shared library outside a reviewed package is not implemented.

Shared native skills require a schema v3 release with `eos_contract = "eos.skill.v2"` and at least one regular `*.skill.so` file. `install` stores the library in its package tree; it does not copy it into `skills/current`. The `skills path|sync` commands remain for the legacy loader and are not the selection mechanism for the runtime manager. Public catalog commands reject private backend releases. `doctor` reports incompatible targets and missing libraries; a MuJoCo package requires its matching separate MuJoCo installation and `MUJOCO_ROOT`.

## Configure an external module

A schema v3 package may contain activation templates. Run `./bin/island-pkg activate NAME [--version VERSION]` to render them into an isolated directory under the package root. The required `module.toml` is written under `etc/ext/<namespace>--<name>/`; it declares package identity, services, and optional EOS bindings. Review that file, select the exact release with `lock add`, and add its path explicitly to the deployment's `etc/runtime/stack.toml` `modules` list. The runtime manager validates and applies the selected graph; `island-pkg activate` does not alter the active stack. Schema v1/v2 releases can still be installed, but their legacy activation layout is rejected with a migration error. The `island/hello` example has no EOS configuration and needs no activation.

```toml
version = 1
eos_config = "../eos/eos.toml"
modules = ["../ext/vendor--driver/module.toml"]
```

The module descriptor uses `version = 1` and `[module]` with `id`, `enabled`, `package`, and `version`. It may declare `[[service]]` and `[[binding]]`; a binding names a backend, profile, and required services. External configuration belongs under `etc/ext/`, while EOS-owned configuration remains under `etc/eos/`. See the [manifest example](../../island/interfaces/packages.md#private-backend-source-packages) and [configuration guide](configure.md).

## Build and remove

`./bin/island-pkg build SOURCE` builds and installs a schema v3 CMake package. `--cmake-option NAME=VALUE` overrides one option; `--cmake-source DIR` points to a separate CMake project. A private backend package must live outside this checkout and can install a bridge executable and a `module.toml` template with its service and binding declarations. Review local CMake source before building because it runs as the calling user. After building, use `lock add` and, if applicable, `activate` as above.

`info NAME [--version VERSION]` inspects a catalog release; `list` shows installed releases; `remove NAME [--version VERSION]` deletes one. Removal refuses a release selected by `isos.lock` or referenced by generated activation configurations. `--root` and `--registry` are global options and precede the command. Package acquisition, runtime binding, and process supervision have separate owners: `island-pkg` handles files and config generation, the EOS runtime manager admits bindings, and `island-launch` owns declared processes.

Developers submit a source repository and GitHub release URL on the catalog page. Reviewers check release assets, license, platform qualifications, and EOS integration before publishing an immutable manifest.
