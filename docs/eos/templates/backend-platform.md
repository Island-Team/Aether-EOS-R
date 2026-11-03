# TODO: platform (system, vendor, ROS 2, simulator)

<!-- docs-tree:start -->
**Location:** [Docs](../../README.md) / [EOS](../README.md) / [Templates](README.md) / **backend platform**

<details>
<summary>Docs tree</summary>

- [docs/](../../README.md)
  - [eos/](../README.md)
    - [templates/](README.md)
      - [backend-module.md](backend-module.md)
      - **backend-platform.md**
      - [builtin-skill.md](builtin-skill.md)
      - [primitive.md](primitive.md)
      - [workflow.md](workflow.md)

</details>
<!-- docs-tree:end -->

## Introduction

Define the platform boundary, supported hardware or process dependencies, build flag, and which side owns device I/O. State whether this platform is included in EOS, optional, or deployment-owned. Link to [backend development](../develop/ext/README.md).

## Modules

| Module | Device class | Transport/dependency | Module page |
| --- | --- | --- | --- |
| `TODO` | `TODO` | `TODO` | `TODO` |

Create a separate [backend module page](backend-module.md) for each adapter, including multiple modules on one platform. For the current inventory see the [backend catalog](../reference/backends/README.md).

## Build and setup

List required libraries, CMake options, permissions, environment setup, and service startup order. Link to [driver setup](../usage/backends.md) and a tested profile example. Keep site-specific endpoints in the profile.

## Shared configuration and behavior

Document admitted backend type names, common TOML keys, units, clocks, health/freshness, result correlation, cancellation, stop/estop, and dependency loss. Link to [configuration](../usage/configuration-reference.md) and the [southbound driver contract](../interfaces/southbound/driver.md).

## Verification and troubleshooting

Give platform-level readiness checks and qualification requirements. Link to [launch](../debug/launch.md) and [logs](../debug/README.md). Update relative paths for the final route.
