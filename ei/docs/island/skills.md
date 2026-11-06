# Native skills and the shared skill boundary

Native skills are deployment-supplied EOS libraries that describe reusable device behavior. Their code is loaded by EOS; their metadata is discovered by EGIA as planning guidance. A native skill is not a JSON workflow, an EGIA command, or a replacement for a typed primitive.

## Where skills live

Each profile has a boundary document at logical `skills/<skill-profile>/` in exactly one configured shared root. The checkout default resolves the Island profile to `../share/skills/island/README.md`; an installed composition resolves it below `share/island/`. Skill implementation and loading are private to the EOS deployment; this repository interacts only through `SkillService`.

## Metadata and composition

A skill exposes a stable SemVer name/version identity, a useful description, self-contained parameter and return JSON Schemas, exact versioned primitive references, and resource URI templates. Native ABI and library provenance are private EOS deployment details. Every listed primitive identity must be discoverable through EOS and retain its canonical request schema, numeric limits, and safety tier.

`SkillService.ListSkills` returns those complete descriptors inline, and `GetSkill` resolves one exact identity. A valid empty catalog is supported; transient service unavailability degrades only the skill view, while a missing or malformed service is incompatible. EGIA may use the metadata to shape a plan, but this stage still invokes primitives only. Operator procedures may compose the supported `arm.init` primitive, but it remains hidden from the planner and requires `[primitives].expose_operator = true`.

## Safety and lifecycle

Argument validation and physical safety remain in the primitive and EOS layers. Skills must not weaken primitive limits or bypass `decision → safety → link → EOS`. Deploy the skill through EOS, restart EOS, and verify discovery before using the skill in a plan.

## Verification

After deployment, verify that EOS reports the complete descriptor through `ListSkills`, that each exact primitive name/version is present in the gateway discovery catalog, and that EGIA exposes the descriptive projection at `/api/aether/egia/skills`. Add contract, discovery, registry, gate, and acknowledgement tests for any new primitive used by a skill.
