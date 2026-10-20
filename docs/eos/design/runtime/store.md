# Runtime private store

[中文](../../../../docs_cn/eos/design/runtime/store_cn.md)

`runtime/store/` owns small typed, EOS-private state. It is deliberately not a general SQL database or arbitrary key/value API. The first collection stores absolute planar navigation points per robot:

```json
{
  "format_version": 1,
  "robots": {
    "robot-0": {
      "navigation_points": {
        "example": {"x_m": 1.0, "y_m": 2.0, "yaw_rad": 0.0}
      }
    }
  }
}
```

The file is `${ISLAND_STATE}/eos/custom-store/navigation-points.v1.json` (or the checkout state fallback). EOS creates the directory as `0700` and writes snapshots atomically through `0600` temporary files, `fsync`, and rename. Names and robot identifiers are bounded path segments; coordinates must be finite. The format rejects unknown fields, unsupported versions, symlinks, oversized files, and more than 10,000 points.

Operators may edit the JSON directly. EOS detects a changed file before any API mutation and returns a conflict instead of overwriting it. After an edit, call `POST /v1/eos/navigation/waypoints:reload` or run `waypoint reload` in the REPL. Invalid edits fail closed and leave the in-memory snapshot unchanged.

Stored points are also readable as `robots/{robot}/navigation/waypoints/{name}`. That projection supplies `reference: absolute` and `frame_id: map` for the existing `navigate_waypoint@1.0.0` skill; only `x_m`, `y_m`, and `yaw_rad` are stored.

The typed API lists, reads, upserts, and deletes points. Last write wins inside one EOS process; concurrent external writers are not supported.
