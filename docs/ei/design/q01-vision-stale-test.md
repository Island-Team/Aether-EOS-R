# Q01 Vision duplicate-generation replay design

[中文](../../../docs_cn/ei/design/q01-vision-stale-test.md) · [Owner interface](../interfaces/vision-evidence.md) · [First use](../develop/build-and-run.md)

Q01 needs a selected running EGIA child to reject a duplicate generation from its current EOS gate stream. An older subscription cursor cannot prove that branch because EOS may legitimately deliver a newer frame on the same stream. The private test seam replays only EGIA's latest already accepted frame into its existing Vision admission body; it cannot choose a new frame or reach the stage, model, mission, or actuator paths.

The test socket is created only after managed run, revision, owner grant, and inspection verifier checks, and only with an inherited manager opt-in. The public inspection WebSocket stays read-only. EGIA keeps one extra copy of accepted metadata in test mode and reuses its existing single transient JPEG. One admission lock serializes ordinary frame acceptance, stream attestation, disconnect, stop, and the replay's before/after snapshots. This makes a later legitimate frame either win first and return `target_changed`, or wait until the duplicate rejection has completed.

Invalid or changed-target requests do not consume the opportunity. A matching replay consumes it when the ordinary admission logic increments only `generation_stale`; the listener is then removed even if the caller loses the response. Disconnect or a new stream drops replay material, while grant loss and Stop also remove the test listener. The selected Q01 process replay and independent implementation review remain necessary before camera step 4a can close. Exact socket and wire fields live only in the [owner interface](../interfaces/vision-evidence.md).
