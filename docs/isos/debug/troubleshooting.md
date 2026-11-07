# Troubleshooting

This page helps operators identify where a package or run failed. Read the structured error's `code`, `stage`, and graph `path`; the [HTTP envelope](../develop/http/envelope.md) defines their meanings. Inspect current state with `isos status --json` and `isos logs all --current` before retrying.

## Package rejection

An unsupported contract, changed payload, unsafe path, unknown resource, owner, or lifecycle adapter fails admission. Use the exact pin and canonical path to find the declaration. Reinstall or relink changed bytes explicitly; a retry does not refresh a receipt. See [package verification](../interfaces/packages/verification.md).

## Launch and readiness

Check the selected owner, process declaration, and bounded readiness result in the logs. A failed candidate does not replace an active run. The [process contract](../interfaces/packages/process.md) and [handoff](../interfaces/cores/handoff.md) identify what IsOS verified and what the owner must accept.

## Logs and uncertain stop

Use the current run ID and [log cursor contract](../develop/owner/log-cursors.md) to keep events from different runs separate. If terminal stop proof is missing, stale, late, or contradictory, status remains `unknown`. Do not start a replacement until the owner and process state are reconciled; see [stop proof](../interfaces/cores/dependent-stop.md).

[中文](../../../docs_cn/isos/debug/troubleshooting_cn.md)
