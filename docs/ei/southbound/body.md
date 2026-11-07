# EOS body discovery

[中文](../../../docs_cn/ei/southbound/body.md) · [Southbound overview](../southbound.md)

`Discover` is the sole EOS body and primitive-binding discovery call. EGIA combines it with `ListResources` and `SkillService.ListSkills`, validates the complete response, and atomically replaces one configured robot alias's generation.

The robot descriptor carries `id`, `name`, `model`, `description`, operational `state`, `health`, emergency-stop state, optional power percentage, and robot-scoped primitive bindings. A robot-scoped binding, including `robot.stop` or `safety.estop`, is called with an empty device ID.

Each device descriptor carries `id`, `class`, `part`, `description`, `state`, `health`, and device-scoped primitive bindings. Backend names, driver versions, endpoints, raw telemetry, configuration internals, and media do not cross this boundary.

Every primitive binding supplies its exact name and version, description, direct or advanced kind, health, supported `relative`/`absolute` references, optional absolute frame, enforced numeric limits, accepted media types, and dependency health. EGIA preserves all valid bindings in its diagnostic catalog. Only bindings whose health is `ready`, whose exact schema is known, and whose local exposure policy permits execution enter the callable registry.

Discovery failures do not replace the last complete generation. Successful rediscovery removes vanished devices, resources, skills, and primitive targets together. The monitor uses `GetStatus.started_at` as the EOS process-generation identity and performs bounded rediscovery so changed health and bindings cannot remain stale.
