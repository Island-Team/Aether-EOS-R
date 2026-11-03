# R06 EOS simulator stop cases

[中文](README_cn.md) · [Contract](../../../docs/eos/interfaces/simulator-stop.md) · [Design](../../../docs/eos/design/runtime/simulator-stop.md)

`cases.json` freezes 59 owner and cross-owner cases for EOS R06. The receipt regression also asserts that a previously ready child retains its verified generation when the stop response is lost, while `stop_epoch` stays null and the outcome stays unknown. `python3 tests/fixtures/simulator_stop/check.py` verifies the inventory, bilingual receipt examples and canonical backend identity only; it does not execute physical stop or establish a safe-stop result. Implementation tests must bind each case to a unit, stub or real child assertion and preserve full command output in `logs/test/<run-id>/`.
