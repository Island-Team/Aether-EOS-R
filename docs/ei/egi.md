# EGI and EGI Agent terminology

Island-egia uses two related names with deliberately different meanings.

## EGI

**EGI** expands to **Embodied General Intelligence**. It names the research direction associated with this runtime; the implementation status and future work are maintained in [todo.md](todo.md).

## EGI Agent

**EGI Agent** is the software runtime implemented by this repository. It hosts configured models and coordinates perception, context, planning, memory, missions, safety-gated primitives, and recovery.

**EGIA** is the compact acronym for **EGI Agent** and supplies the `egia` suffix in the repository name **Island-egia**. Use **EGI Agent** in prose because it is immediately understandable; use **EGIA** where a short identifier is useful. EGIA names the runtime.

The relationship is:

```text
configured model → EGI Agent → embodied runtime/backend → action
```

The model supplies learned intelligence. The EGI Agent supplies the agent loop and cognitive orchestration. The embodied runtime/backend owns device integration, execution, and final physical safety.

## Technical names

The repository and its technical interfaces use **EGIA** consistently:

- `egia` Python packages and executable
- `EGIA_*` environment variables
- `etc/egia`, `docs/egia`, and related installed paths
- `egia_ext` for bundled out-of-process extensions

This is a breaking rename from the former identifiers; deployments and downstream integrations must update imports, commands, configuration paths, and environment variables together. In prose, use **EGI Agent** for the runtime and expand **EGI** as **Embodied General Intelligence** on first mention.
