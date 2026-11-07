# EOS skill discovery

[中文](../../../docs_cn/ei/southbound/skills.md) · [Southbound overview](../southbound.md)

After mandatory body and resource discovery, EGIA calls `SkillService.ListSkills`. Each descriptor is keyed by exact name and version and includes its description, parameter and result schemas, exact primitive dependencies, and readable resource templates. Native ABI, library, provider, and build details stay private to EOS.

An empty admitted catalog is valid. A transient `UNAVAILABLE` response makes the skill view degraded without invalidating an otherwise complete body/resource generation. A missing service, malformed or duplicate descriptor, invalid JSON Schema, or dependency absent from gateway discovery makes the peer incompatible and prevents that generation from publishing. Prompts show only compositions whose primitive dependencies are visible under EGIA policy.

The generated transport also contains asynchronous execution RPCs. This integration stage uses the service for catalog discovery only; governed execution is introduced separately.
