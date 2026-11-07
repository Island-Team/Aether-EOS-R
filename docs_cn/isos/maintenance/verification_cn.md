# 验证

本页面向检查当前 IsOS 源码树的贡献者。将完整命令输出保存在忽略的 `logs/test/<run-id>/` 下；每次运行记录源码提交和工具链。

```sh
cargo fmt --all -- --check
cargo clippy --all-targets -- -D warnings
XDG_CONFIG_HOME=/tmp/isos-generic-node-empty cargo test --all-targets -- --test-threads=1
python3 tools/check_isos_docs.py
python3 tests/fixtures/control_grants/check_cases.py
python3 tests/fixtures/reader_features/check_docs.py
git diff --check
```

2026-11-05 的通用节点运行通过格式检查、严格 Clippy、324 个单元测试及 24 个集成测试；1 个依赖归属方的测试被忽略。两个无关包家族的聚焦测试还覆盖注册声明、选择器 ID、类型化边、资源变更、不安全路径和过大元数据。控制授权和读取器功能文档夹具均通过。文档检查发现英文与中文各 55 页，目录、标题、相对链接、锚点及 JSON 示例均无问题。完整输出位于 `logs/test/2026-11-05-generic-node/`。临时空白 `XDG_CONFIG_HOME` 让测试不依赖操作者的有效 IsOS 配置。洁净主目录下的归属方回放在 `logs/test/20261105-generic-node-bridge/replay-r29.log` 中通过了两个模拟器场景：通用包安装、图 Validate 和 Apply、Run 就绪、经 EOS 传递的 IsOS 按键按下／续租／释放、每场景 20 帧相机图像、完成的原语命令，以及模拟器已回收的确认 Stop。该回放使用的 EOS 与包侧改动仍为本地未提交状态；其余负例和浏览器场景见[待办](todo_cn.md)。

2026-11-06 的配置验证运行了 `cargo test --all-targets --no-fail-fast`、`cargo clippy --all-targets -- -D warnings`、`cargo fmt --all -- --check`、`python3 -m unittest discover -s tests -p 'test_install_user_service.py'`、`python3 tools/check_isos_docs.py` 和 `git diff --check`，全部通过。文档检查发现两种语言各 56 页、零问题。还针对选定的本地主目录运行了已安装的 `isos --help` 和 `isos version check --json`。完整 Cargo 与安装器输出位于 `logs/test/2026-11-06-config/`。

源码优先级检查运行 `python3 -m unittest discover -s tests -p 'test_source_launcher_env.py'`，验证 `.env` 优先于 `.env.example`，而示例会补充缺失值。配置单元测试验证选定 TOML 优先、示例补充缺失字段。

[English](../../../docs/isos/maintenance/verification.md)
