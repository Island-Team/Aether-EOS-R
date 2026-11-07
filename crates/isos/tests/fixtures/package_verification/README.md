# P01 frozen verification cases

These design fixtures support the [IsOS verification contract](../../../../../docs/isos/interfaces/packages/verification.md). `cases.json` fixes the exact pin, operation, mutation, classification and graph/process invariants. The implementation test builder creates a tiny executable, descriptor, scene, asset and v4 release with computed hashes under fresh `ISOS_HOME`; placeholder digests are never accepted. Each negative case mutates one byte, mode, link or identity after creating the positive bundle. Cases marked `recheck` run through Apply and Run as well as package APIs.

For every failure assert no partial store publication, unchanged saved graph bytes/revision, unchanged active revision and child PID, and sanitized response/log without source path or raw stderr. On success assert receipt schema 2, exact release/payload/sidecar digests, owner-only metadata, pinned sidecar and no deployment activation. An unrelated corrupt installed package cannot affect a graph that does not pin it. P02 owns semantic scene/device and MuJoCo library cases.

[中文契约](../../../../../docs_cn/isos/interfaces/packages/verification_cn.md)
