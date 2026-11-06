# P07 current-run log cursor fixtures

[中文](README_cn.md) · [Owner interface](../../../../../docs/isos/develop/owner/log-cursors.md) · [Design](../../../../../docs/isos/design/server.md)

`cases.json` freezes the opt-in endpoint, response keys, scope and anchor fields, filter order, diagnostic order, bounds and 22 positive/negative wire cases and twelve executable state scenarios. Key rotation keeps the independently proven last run while rejecting old tokens and marking its anchor and collector continuity unavailable. A tampered `r812` → `r999` scope fails closed. `check_cases.py` executes a reference admission/scope selector over the state scenarios and checks wire fixture consistency with `python3 tests/fixtures/log_cursors/check_cases.py`; it is contract evidence, not a runtime test. Implementation tests must drive real LogStore/HTTP/CLI paths and compare observable pages and status with these cases. The `lc1.*` values in queries are scenario labels, not serialized tokens.
