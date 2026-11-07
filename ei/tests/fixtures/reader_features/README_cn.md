# P05 EGIA 读取器夹具

[English](README.md) · [归属契约](../../../../docs_cn/ei/interfaces/reader-features.md) · [设计](../../../../docs_cn/ei/design/reader-features.md)

`advertised.json` 冻结所选 `egia-config` schema-1 身份和 `egia --capabilities` 运行时回复，包含四个精确且排序的旧版 EGIA 设计 ID 及其位置。运行时特意不声明图／类型数组、阶段或就绪。`cases.json` 冻结 22 项正例、负例及拒绝时不停止的设计用例。公共 `{code,path,message,stage,reader}` 诊断由 IsOS 持有；EGIA setup 持有现有 `{path,code,message}` 诊断，失败时 `files` 为空。EGIA setup 回复路径包含 `/graph`；IsOS 公共路径不包含。

使用 `python3 -m json.tool tests/fixtures/reader_features/advertised.json` 和 `python3 -m json.tool tests/fixtures/reader_features/cases.json` 校验两个 JSON 文件，再运行 `python3 tests/fixtures/reader_features/check_cases.py`。实现应将实际所选探针与 `advertised.json` 比较，并通过同一个所选解释器及 IsOS 预检回放 `cases.json`。该设计夹具不构成运行时证据。实现后把完整输出保存在忽略 Git 的 `logs/test/<run-id>/`。
