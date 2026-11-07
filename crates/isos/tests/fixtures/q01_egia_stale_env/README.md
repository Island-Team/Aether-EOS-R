# Q01 EGIA test environment guard fixtures

The two cases are graph-authored settings for the IsOS-owned `egia` process node. Validate and Apply must reject each at the exact JSON Pointer with `egia_test_reserved_env`, before writing graph bytes or starting a child. `env_from` sources are not reserved by this guard; only the target key is interpreted by EGIA. A manager-inherited `EGIA_Q01_STALE_TEST=1` remains available for a disposable selected run.
