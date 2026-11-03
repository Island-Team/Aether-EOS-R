# P05 EOS 读取器夹具

[English](README.md) · [归属方契约](../../../docs_cn/eos/interfaces/reader-features_cn.md) · [设计](../../../docs_cn/eos/design/runtime/reader-features_cn.md)

`advertised.json` 冻结四个所选 EOS 可执行文件的探针、现有特性顺序、P05 新增的 `mujoco-child-v2`、11 个精确旧版及七个精确新版 EOS 内建类型 ID，以及 `aether-profile setup --json` 身份必须与 `aether-profile --capabilities` 共享的字段。静态类型列表不包括软件包贡献和其他 core。对软件包贡献类型，需将 P01 已验证 sidecar 提供给所选 profile 工具，并检查有效 catalog 和归属方校验器。

`cases.json` 冻结图／特性推导的正例，以及身份、旧版标量、未来版本、精确类型、软件包和所选二进制文件的负例。它覆盖全部三种实际 MuJoCo v2 子进程启动身份、没有 camera gate 的已启用 bridge，以及两种旧版子进程形式和 bridge 缺失特性的负例。`expected` 是可机器读取的结果类别；IsOS 归属负例的 `code`、`stage` 和 `path` 与其 P05 归属方契约对齐。P01/P02 篡改与 G01 未知图／类型拒绝优先于 P05 读取器诊断。EOS setup 失败保留 schema-1 `{path,code,message}` 与空 `files`。

用 `python3 -m json.tool tests/fixtures/reader_features/advertised.json` 和 `python3 -m json.tool tests/fixtures/reader_features/cases.json` 校验两个 JSON 文件。契约测试应将实际构建的二进制文件和 setup 身份与 `advertised.json` 比较，再通过 IsOS 预检运行图用例。仅设计夹具不构成实现证据。P05 实现时将测试输出和所选二进制文件哈希保存于 `logs/test/<run-id>/`。
