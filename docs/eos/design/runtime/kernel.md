# Hardware capability kernel

<!-- docs-tree:start -->
**Location:** [Docs](../../../README.md) / [EOS](../../README.md) / [Design](../README.md) / [Runtime](README.md) / **kernel**

<details>
<summary>Docs tree</summary>

- [docs/](../../../README.md)
  - [eos/](../../README.md)
    - [design/](../README.md)
      - [runtime/](README.md)
        - [northbound/](northbound/README.md)
        - [skills/](skills/README.md)
        - [actionlist.md](actionlist.md)
        - [dispatch.md](dispatch.md)
        - [hardware.md](hardware-legacy.md)
        - [init.md](init.md)
        - **kernel.md**
        - [models.md](models.md)
        - [northbound.md](northbound.md)
        - [perception.md](perception.md)
        - [skills.md](skills.md)
        - [store.md](store.md)

</details>
<!-- docs-tree:end -->

[中文](../../../../docs_cn/eos/design/runtime/kernel_cn.md) · [Runtime overview](README.md) · [Middleware map](../eos-middleware-map.md)

The **EOS kernel** is an application-level admission layer, not an operating-system kernel. This first implementation adds a hardware [Catalog and Broker](../../../../adapters/eos-cxx/runtime/kernel/) between trusted runtime callers and the existing [Dispatcher](dispatch.md). Dispatcher retains schema validation, command identity, ordering, leases, stop and estop behavior, and the only backend release path.

## Catalog and names

At startup, `Catalog` projects every primitive declared on an admitted device into one canonical name: `hardware/{robot_id}/{device_class}/{part_id}/{primitive}@{version}`. Each segment is percent-escaped as needed. The stable robot and part identities identify the intended hardware, while the catalog resolves the generated process-local `device_id` before dispatch. Implicit device `motion.stop` uses the same form; whole-runtime `robot.stop` and `safety.estop` use `runtime/{robot_id}/{primitive}@1.0.0`. Duplicate canonical names fail startup. The catalog can also accept named deployment slots through the existing hardware resolver, which fails closed for missing, ambiguous, or undeclared targets; current `eosd` composition passes no extra slots.

The catalog's `Available` check reads current device health and requires both `alive` and `admitted` for ordinary device actions; stop and estop capabilities remain available. It supports live [v2 discovery](../../interfaces/capabilities.md) and grant issuance; it is not an extra gate for an already admitted command. Dispatcher owns command replay, safety and health decisions, command validity, and physical release. The catalog currently contains hardware operations only; resource, model, skill, and community capabilities remain later stages of the [middleware map](../eos-middleware-map.md).

## Broker and callers

The Broker resolves a hardware capability and hands the exact primitive, version, device ID, arguments, and command ID to Dispatcher. For a worker-style caller it can issue an opaque grant bound to one capability target, `execution_id`, generation, and expiry; grant issuance requires current availability. Execution requires the same scope and a valid unrevoked token; revocation can target an execution or generation. A capability name alone grants no authority. Once a command has been admitted, Broker forwarding must leave terminal replay and current safety or health outcomes to Dispatcher.

The current runtime composes one Catalog and Broker after hardware admission. ActionList capability execution and trusted internal primitive execution enter through the Broker. The v2 [CapabilityService](../../interfaces/capabilities.md) resolves a canonical name through the same Broker and starts a Dispatcher command whose ID is the operation ID. Get, watch, and cancel read Dispatcher command records; a recovered `Operation` cannot reconstruct the caller's capability alias. The separate HTTP adapter exposes the v2 capability routes as REST and SSE. Production packaged workers use this same Broker: the provider resolves each declared primitive, version, and admitted device to its canonical catalog name, then requests an opaque grant for that exact capability and execution scope. Worker primitive start, poll, and cancel requests return through the Broker before reaching Dispatcher. The older standalone worker-provider constructor with a private grant table remains only for isolated tests. Trusted entry points are selected by EOS composition, never by a request-supplied flag.

## Failure and migration boundary

Unknown or unadmitted hardware operations fail before dispatch, and unavailable devices cannot receive new grants. A missing, expired, or revoked scoped grant fails authorization. A trusted caller remains subject to Dispatcher admission, safety, health, and replay rules; the Broker does not retry a command or synthesize a successful acknowledgement. The current northbound transport has no caller authentication, so deployment still relies on its documented listener boundary; the trusted method must not be exposed as an unguarded worker or external-call option.

HTTP, REPL, ActionList workflows, and agents can use the versioned capability action surface. Other operation owners remain future work. The Broker itself remains an internal C++ boundary; its public contract is the v2 gRPC and HTTP service.
