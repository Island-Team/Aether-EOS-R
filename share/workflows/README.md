# Workflow templates

EOS accepts workflow templates through its northbound workflow registry. A
workflow names installed skills and their exact versions; each robot deployment
supplies its own device IDs, waypoints, resource URIs, and safety limits.

No physical-robot workflow templates are bundled with the public release.
Create templates for your own deployment after validating its profile and
calibration. See the [workflow interface](../../docs/eos/develop/interfaces/workflow.md)
for the registry contract.
