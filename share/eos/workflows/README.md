# Deployment workflow templates

EOS loads `*.json` files in this directory as `WorkflowDefinition` templates at startup and exposes them through `WorkflowService`. A malformed template prevents EOS from becoming ready. Registered templates are not submitted until an operator calls `workflow run <template> [workflow-id]` through the REPL or uses the corresponding service API.

Templates are deployment specific. Keep them in this local, Git-ignored directory and provide admitted robot and device IDs before running them. The tracked repository intentionally ships no executable workflow templates.

Copy [`examples/workflows/hello.json.example`](../../examples/workflows/hello.json.example) to the ignored `hello.json` after installing the matching `example.hello` skill scaffold. See the [local deployment guide](../../docs/eos/usage/configure.md).

An action with `executionMode: "ACTION_EXECUTION_MODE_MONITOR"` must declare `monitorLifetime: "MONITOR_LIFETIME_WORKFLOW"`. Independent monitors can instead be supervised through `SkillService`.
