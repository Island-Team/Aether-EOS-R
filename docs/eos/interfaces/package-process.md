# Package process handoff to EOS

[中文](../../../docs_cn/eos/interfaces/package-process_cn.md) · [Interface catalog](README.md) · [MuJoCo adapter](mujoco.md) · [JSONL events](events.md)

IsOS owns package verification, graph admission, and run coordination. It passes a verified package process to `aether-runtime` with `--package-process <absolute-json-file>`. EOS owns the child, readiness, stop proof, reap, and adapter dispatch. This v1 handoff admits one process. Legacy `--backend-*` flags remain available for older managed graphs and cannot be combined with this option.

```json
{
  "schema_version": 1,
  "node_path": "eos/process/backend/simulator",
  "node_type": "eos.process.backend@v1.0",
  "package": {"id": "island/mujoco-sim", "version": "0.1.1"},
  "entrypoint": "/var/lib/isos/packages/island/mujoco-sim/0.1.1/bin/simulator",
  "resources": {"scene": "/var/lib/isos/packages/island/mujoco-sim/0.1.1/assets/scene.xml"},
  "lifecycle_adapter": "mujoco-v2",
  "release_sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  "payload_sha256": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
}
```

`schema_version` is an integer with no unit; only `1` is accepted and no default exists. `node_path` is a nonempty graph path string, and `node_type` is a nonempty registered type string; both must match the selected graph node. `package.id` and `package.version` are nonempty strings with no unit and must equal that node's exact package pin. `entrypoint` is an absolute path string with no unit and must be an executable regular file. `resources` is a map of resource names to absolute path strings with no unit; the selected adapter defines accepted names and validates their files. The current `mujoco-v2` adapter requires exactly `scene`, a regular file matching the selected scene. IsOS resolves these paths only after verifying the package and selected resource; EOS validates them again before spawn. `lifecycle_adapter` is a string with no unit naming the EOS adapter; v1 registers only `mujoco-v2`, which checks the MuJoCo device, socket, and scene bindings. IsOS must reject any other adapter before Apply or Run. `release_sha256` and `payload_sha256` are lowercase 64-character SHA-256 hex strings, with no unit; IsOS supplies digests from its verified receipt and EOS includes them in its `package_admitted` event. Every field is required, unknown top-level fields are rejected, the JSON file is limited to 64 KiB, and missing or invalid data fails startup before the child is launched. Future schema versions and adapter families require explicit reader support.

IsOS supplies a run ID with `--run-id` and the EOS log directory with `--log-dir`. The package child's stdout and stderr go to `package-process.log`; they are raw child output, not authoritative EOS events. EOS emits bounded JSONL lifecycle and failure events on its own stderr using the [event contract](events.md). IsOS validates the producer and run identity before adding events to the combined run log. Readiness requires the selected adapter's live health proof. On stop, EOS fences admitted actuators, obtains physical stop proof, and only then terminates and reaps the child. Missing proof yields an unknown terminal outcome; it is never reported as confirmed.
