# P05 EGIA reader fixtures

[中文](README_cn.md) · [Owner contract](../../../../docs/ei/interfaces/reader-features.md) · [Design](../../../../docs/ei/design/reader-features.md)

`advertised.json` freezes the selected `egia-config` schema-1 identity and `egia --capabilities` runtime reply, including four exact sorted legacy EGIA design IDs and their placements. The runtime intentionally has no graph/type arrays or stage/ready claim. `cases.json` freezes 22 positive, negative and no-stop design cases. IsOS owns public `{code,path,message,stage,reader}` diagnostics; EGIA setup owns its existing `{path,code,message}` diagnostic and empty files on failure. The paths in EGIA setup replies include `/graph`; IsOS public paths do not.

Validate both JSON files with `python3 -m json.tool tests/fixtures/reader_features/advertised.json` and `python3 -m json.tool tests/fixtures/reader_features/cases.json`, then run `python3 tests/fixtures/reader_features/check_cases.py`. The implementation should compare actual selected probes with `advertised.json` and replay `cases.json` through the same selected interpreter and IsOS preflight. This design fixture is not runtime evidence. Save complete output under ignored `logs/test/<run-id>/` after implementation.
