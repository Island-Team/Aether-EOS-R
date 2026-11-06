# 发行测试 EOS 目录样例

[English](README.md)

`catalog.json` 是从 EOS `master` 提交 `25c2d64c420d7ae2402bd386ebb1cdb1b31f1a1c`、工作区版本 `0.2.0` 构建的 `aether-profile catalog` 精确输出。其 SHA-256 为 `eda80c78e9b414249a8aca89756ae30b142ad03f3f61e7a792f9d438c0bd78d4`。从该精确源码运行 `cargo build --locked -p aether-runtime --bin aether-profile` 及 `aether-profile catalog` 重新生成，并在修改样例前比较摘要。

此处的 `aether-profile` 是 IsOS 保存图、所选目录和 P01 重新校验测试的窄范围可执行桩。它接受 `catalog`，以及一个贡献数组为空的 `--node-types` sidecar；其他命令或非空贡献会被拒绝。发行工作流仅在 `cargo test` 时通过只在 `#[cfg(test)]` 生效的 `ISOS_TEST_CATALOG_PROFILE` 钩子选择它。钩子校验绝对、可执行路径；文件不存在即失败。生产版 `core_bins::resolve("aether-profile")` 及运行准入仍要求已安装的 EOS 归属方。真实 EOS 目录、sidecar 与就绪集成由 Q04 所选归属方重放另行验证。

前几次 CI prepare 运行未选择 EOS 归属方，而开发机有相邻的 EOS 程序。该测试样例使单元测试不再依赖相邻程序，同时不宣称软件包贡献适配器已编译。保留归属方缺失时的只读和 503 断言；生产二进制不得用此样例代替归属方故障。
