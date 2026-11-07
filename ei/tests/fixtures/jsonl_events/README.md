# EGIA P06 event fixtures

[中文](README_cn.md) · [Owner interface](../../../../docs/ei/interfaces/events.md) · [Design](../../../../docs/ei/design/events.md)

`egia-role-policy.json` is the normative machine-readable list of the 16 EGIA codes, fixed messages, severities (including outcome-dependent terminal and stop-state severity), node rules and typed field/enum domains. IsOS must pin this reviewed catalog into its selected-child role policy before accepting EGIA stderr. `cases.json` names 45 positive, rejection, suppression and outage cases for implementation tests, including all four `egia_stopped` state/level pairings. `python3 tests/fixtures/jsonl_events/check_cases.py` validates the catalog and representative ID, role, field and byte boundaries. The checker is design evidence, not a runtime emitter test.

Implementation should replay every named case with fake clocks, stub EOS receipts, isolated private replay and captured managed stderr. Then select an actual EGIA child under IsOS and compare the same accepted event in `egia.log` and `events.log`, verify the first failure and terminal event, and prove owner replay survives collector failure. A startup failure before EGIA can safely emit belongs to IsOS supervision. Preserve full output under ignored `logs/test/<run-id>/` and summarize commands/results in the design page.
