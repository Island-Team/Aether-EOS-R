# EI agent guide

Follow the repository [conventions](../docs/island/conventions.v1.3.1.md), the root [AGENTS.md](../AGENTS.md), and the current [EI documentation](../docs/ei/README.md). EI is the intelligence and local safety owner inside the unified IsOS source tree. IsOS owns configuration, applied graph revisions, package admission, and process supervision. EOS owns Dispatcher and actuator admission. Keep EI independently runnable for focused tests.

The Python project lives in `ei/`; its current source and import-rule layout is defined by `pyproject.toml`. Some implementation and compatibility names still say EGIA. Remove those only in a coordinated contract migration, not as an incidental path move. Keep generated protobuf modules synchronized with the in-repository contract source. Put real hardware identities, calibration, endpoints, and field-debug evidence only in private `isos-p` or a more restricted private store.

Before committing EI behavior changes, run Ruff, Pyright, import rules, focused and full tests as applicable, and update English/Chinese owner documentation together. Keep complete logs under ignored `logs/test/<run-id>/`. Preserve unrelated edits and report any module ownership conflict before choosing a target.
