# P07 IsosUI log cursor fixtures

[中文](README_cn.md) · [UI contract](../../../../docs/isosui/interfaces/log-cursors.md) · [Design](../../../../docs/isosui/design-log-cursors.md)

`cases.json` freezes 31 UI state and service-boundary cases. `check_log_cursors.py` validates stable IDs, bounds, exact P07 query/response names, required safety assertions and English/Chinese documentation links. These fixtures specify target behavior, not implemented UI. Implementation tests should turn every case into a reducer or stub HTTP replay, including an actual IsOS rotation and collector recovery replay. Save complete output under ignored `logs/test/<run-id>/`.
