# Outstanding work

This list is for contributors tracking behavior that the current IsOS build has not yet qualified. The [live contracts](../interfaces/README.md) describe implemented inputs; [verification](verification.md) records checks already run.

## IsOS gates

- [ ] Consolidate graph admission, Apply, Run, and Restart into one typed plan carrying all verified owner, resource, readiness, stop, and run fences. The current wrapper and supervisor hold these values separately.
- [ ] Test two unrelated synthetic package families through install, options, Apply, Run, Restart, removal, and rejection. Include malformed or oversized metadata, changed files, unknown resources or owners, stale revisions, failed readiness, uncertain stop, and shutdown.
- [ ] Qualify owner descriptor registration and port/type binding before enabling a successful presentation selection. The current route returns 503 for a valid active output with no descriptor.

- [ ] Extend the successful clean-home keyboard-to-base replay with browser focus-loss and disconnect, owner lease expiry, stale-run rejection, and failed-stop recovery. Press, renewal, release, and confirmed Stop passed with the verified simulator package; see [verification](verification.md).

## External integration gates

- [ ] Package publishers provide `isos.package@v1` contributions with registered types, processes, resources, and node sets. Owners validate their own payload formats and lifecycle adapters.
- [ ] Process owners accept the bound `isos.package-process@v1` handoff and prove readiness and terminal stop. Current setup, event, and stop wires retain their documented owner-specific schemas until their owners migrate them.
- [ ] Data owners register verified public descriptors with exact type, output port, representation, payload bound, endpoint identity, run ID, and graph revision.
- [ ] IsosUI uses `/api/isos/v1/*`, checks `api_schema_version:"0.1.0"`, and invalidates subscriptions when run or revision changes.
- [ ] With migrated owners, replay install, options, Validate, Apply, Run, Restart, presentation, logs, Stop, and failed-stop recovery. Local stubs alone do not establish live interoperability.

[中文](../../../docs_cn/isos/maintenance/todo_cn.md)
