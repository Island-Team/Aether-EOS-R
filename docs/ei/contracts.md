# `egia.contracts` validation boundary

[中文](../../docs_cn/ei/contracts.md)

## Contents

- [Overview](#overview)
- [Architecture](#architecture)
- [Owned modules](#owned-modules)
- [Data flow](#data-flow)
- [Conditions](#conditions)
- [Related documentation](#related-documentation)

## Overview

`egia.contracts` turns shared EOS JSON documents and protobuf payloads into strict EGIA types. It is the semantic boundary between untrusted southbound data and the runtime. Contract modules do not discover devices, choose targets, or execute primitives.

## Architecture

```mermaid
flowchart LR
  Roots["Logical contracts/ and primitives/ roots"] --> Source["source.py schema registry"]
  EOS["EOS payloads"] --> Validate["Canonical schema plus typed validation"]
  Source --> Validate
  Validate --> Types["Body descriptors, world snapshots, actions, acknowledgements"]
  Types --> Core["link, perception, registry, and safety"]
```

## Owned modules

`source.py` resolves JSON documents through collision-checked `SharedResources`, loads every schema, and builds the cross-schema registry. `validate.py` applies canonical JSON Schema and Pydantic validation. `registry.py` maps resource and primitive kinds to schema paths and Python models. `endpoints.py` classifies world-resource URIs and declares supported resource and primitive kinds. `limits.py` parses numeric capability intervals and checks invocation arguments.

`actions/commands.py` and `actions/emergency.py` define typed primitive arguments; `actions/ack.py` defines the versioned acknowledgement. `perception/body/` owns the class-specific discovery values for base, camera, gimbal, gripper, manipulator, microphone, and speaker plus robot status. `perception/world/` owns scene, vision, pose, VAD, and ASR snapshots. `schemas/northbound/` contains generated EGIA-owned wire schemas and is not a shared EOS schema source.

## Data flow

Startup scans JSON below logical `contracts/` and `primitives/` and registers every non-empty `$id`. A southbound object is checked against its canonical schema and then converted to its typed model. Discovery combines protobuf common fields with the body contract selected by `device_class`; world resource payloads select their model by URI and discriminator.

Primitive discovery maps the exact `{name, version}` descriptor to the corresponding action schema. The descriptor version must be SemVer and equal that schema's `x-island-primitive-version`. Before egress, portable argument shape is validated and numeric values are checked against the selected device limits. EOS acknowledgements are validated, including `primitive_version`, before becoming safety verdicts or operator-visible results.

Camera frames use protobuf rather than JSON Schema. Link validation enforces admitted camera identity, requested count, JPEG content, dimensions, distinct frames, and optional source timestamp before the data reaches a consumer. Inline skill parameter and return values are self-contained JSON Schema documents carried as validated discovery metadata; EGIA does not execute the skill.

## Conditions

A normal payload produces exactly one typed body descriptor, world snapshot, command, or acknowledgement. Nullable world fields stay `null`; absence is not converted into healthy data. Body `configuration` and `operational_state` fields required by the selected class must be present, and explicit unknown values remain null only where that schema permits them.

Configuration fails when resources are used before `configure`, a logical file is missing, JSON is malformed or not an object, a schema lacks `$id`, two schemas reuse an `$id`, or a Draft 2020-12 registry cannot be built. Payload validation fails on canonical-schema or typed-model mismatch. Unknown device classes, primitive kinds or versions, resource kinds, and schema names are rejected rather than passed through.

Numeric ranges reject booleans and non-numbers, non-finite endpoints, ranges without an endpoint, exclusivity without its endpoint, empty intervals, missing required limit names, and unknown limit names. Invocation rejects an argument whose capability was not advertised or whose value falls outside the admitted interval.

Accepted acknowledgements require null rejection fields. Rejected acknowledgements require a non-empty `reason` and a canonical `reason_code`; all acknowledgements require an exact SemVer `primitive_version`. `speaker.play` rejects missing stream identity, invalid media types or base64, oversized chunks, negative sequence values, and unknown fields.

World contracts enforce their semantic invariants, including timestamps, fixed pose array sizes, ordered speech segments, and discriminators. Unavailable EOS transport and timeouts are handled by `egia.core.link`; restart rebuilds the schema registry from configured roots. This package owns no background task and needs no shutdown cleanup.

## Related documentation

See [southbound.md](southbound.md) for the EOS interface, [northbound.md](northbound.md) for generated northbound schemas, [core/link.md](core/link.md) for transport, [core/perception.md](core/perception.md) for admission, [core/registry.md](core/registry.md) for target binding, and [Island shared contracts](../island/contracts.md) for resource-root ownership.
