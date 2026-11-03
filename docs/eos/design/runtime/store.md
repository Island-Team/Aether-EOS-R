# Runtime private store

<!-- docs-tree:start -->
**Location:** [Docs](../../../README.md) / [EOS](../../README.md) / [Design](../README.md) / [Runtime](README.md) / **store**

<details>
<summary>Docs tree</summary>

- [docs/](../../../README.md)
  - [eos/](../../README.md)
    - [design/](../README.md)
      - [runtime/](README.md)
        - [northbound/](northbound/README.md)
        - [skills/](skills/README.md)
        - [actionlist.md](actionlist.md)
        - [dispatch.md](dispatch.md)
        - [hardware.md](hardware-legacy.md)
        - [init.md](init.md)
        - [kernel.md](kernel.md)
        - [models.md](models.md)
        - [northbound.md](northbound.md)
        - [perception.md](perception.md)
        - [skills.md](skills.md)
        - **store.md**

</details>
<!-- docs-tree:end -->

[中文](../../../../docs_cn/eos/design/runtime/store_cn.md)

`runtime/store/` owns small typed, EOS-private state. It is deliberately not a general SQL database or arbitrary key/value API. The first collection stores absolute planar navigation points per robot:

```json
{
  "format_version": 1,
  "robots": {
    "robot-0": {
      "navigation_points": {
        "Entry_in": {"x_m": 1.2, "y_m": -0.04, "yaw_rad": -0.02}
      }
    }
  }
}
```

The file is `${ISLAND_STATE}/eos/custom-store/navigation-points.v1.json` (or the checkout state fallback). EOS creates the directory as `0700` and writes snapshots atomically through `0600` temporary files, `fsync`, and rename. Names and robot identifiers are bounded path segments; coordinates must be finite. The format rejects unknown fields, unsupported versions, symlinks, oversized files, and more than 10,000 points.

Operators may edit the JSON directly. EOS detects a changed file before any API mutation and returns a conflict instead of overwriting it. After an edit, call `POST /v1/eos/navigation/waypoints:reload` or run `waypoint reload` in the REPL. Invalid edits fail closed and leave the in-memory snapshot unchanged.

Stored points are also readable as `robots/{robot}/navigation/waypoints/{name}`. That projection supplies `reference: absolute` and `frame_id: map` for a navigation skill; only `x_m`, `y_m`, and `yaw_rad` are stored.

The typed API lists, reads, upserts, and deletes points. Last write wins inside one EOS process; concurrent external writers are not supported.
