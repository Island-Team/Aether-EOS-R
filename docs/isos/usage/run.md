# Run and stop a deployment

This procedure is for an operator with a saved graph and verified exact package pins. **Run** starts a candidate tied to the saved revision; a **run ID** identifies its lifetime. The owner must report readiness before the candidate becomes active.

Validate the draft, Apply with the current revision, and Run. Recheck status and logs after admission. Stop through the [runtime API](../develop/http/runtime.md) or CLI; IsOS waits for the selected owner's terminal proof and for its own direct children to exit. An uncertain physical stop remains `unknown` and blocks safe replacement. Use [troubleshooting](../debug/troubleshooting.md) before retrying.

The [owner stop contract](../interfaces/cores/dependent-stop.md) defines the current proof fields. IsOS cannot infer a successful stop from a timeout or a disconnected owner.

[中文](../../../docs_cn/isos/usage/run_cn.md)
