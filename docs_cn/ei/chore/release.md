# 发布检查表

[English](../../../docs/ei/chore/release.md) · [公开发布流程](public-release.md)

本文是发布前检查表。独立的 `Aether-EGIA-R` 源码快照流程见[公开发布文档](public-release.md)。EOS 2 capability operation adapter 已在开发分支迁移完成，但必须针对精确的公开 EOS release 通过真实进程集成测试后才能发布。所有项目通过后才能切版。

## 清洁性

- [ ] 公开快照中移除仅用于开发的 `scripts/ssh-tunnel.sh` 和 `scripts/ssh-tunnel.py`。
- [ ] 审查 `var/`，确认 session 输出和本地状态没有进入 Git 或 wheel。
- [ ] 检查 `.gitignore` 和 `git status --ignored`，确认 `.env`、凭据、抓取文件、机器专用配置及生成物未被强制加入。
- [ ] 完成 Apache-2.0 源码授权审查，保留第三方声明，并排除私有扩展和部署数据。

## 包完整性

- [ ] wheel 包含生成的 protobuf binding 和 EGIA 自有的 `egia/contracts/schemas/northbound/` JSON Schema，不包含 EOS 自有 Schema 副本。
- [ ] wheel 不包含测试（`*.tests`、`*.tests.*`）或私有 `libexec/egia_ext/`。
- [ ] 公开快照在缺少私有 TUI 的交互终端上仍可通过 `bin/egia` 启动核心运行时。

## 门禁

- [ ] 全量 `uv run pytest` 通过。
- [ ] `uv run ruff check .`、`uv run pyright` 和 `PYTHONPATH=src:libexec uv run lint-imports` 通过。
- [ ] `PYTHONPATH=src:libexec uv run python -m island_admin.schema --check`、`./bin/islandctl validate` 和规范 Schema 对照通过。
- [ ] EGIA 通过 EOS 2 `CapabilityService` 执行动作，同时保留 EGIA 安全准入；生成的 protobuf、动作版本和 Schema 路径匹配精确 EOS 发布提交。
- [ ] 使用真实 EOS 验证操作提交、重复 ID、终态确认、超时、取消、skill 发现和资源流。
- [ ] `scripts/print_prompt.py` 和 `--json` 可从其他工作目录运行，覆盖所有 lane 且不包含已删除的合成上下文。
- [ ] `etc/egia/missions.toml` 严格加载、依赖图无环、staged install 包含该文件，planner prompt 不含比赛专属指令。
- [ ] 公开 Mission 模板不包含凭据、私有 endpoint、机器路径、个人数据或未经授权的第三方材料。
- [ ] EGIA-only CMake configure、`egia-dist` 与 staged install 成功，共享根合并无冲突，生成的 `island.toml` 选择 `share/island`。
- [ ] 默认 `EGIA_INSTALL_PRIVATE_EXTENSIONS=OFF` 的 staged install 不含 `libexec/egia_ext/`，但保留 `island_admin`。
- [ ] IsOS 使用 `--managed --no-tui` 启动 EGIA；`[launch] sandbox = true` 在启动内部 sandbox 前被拒绝。

## 配置与状态

- [ ] 缺失配置根时，启动错误包含解析后的路径，不使用编译进程序的 fallback endpoint。
- [ ] `bin/island` 转发到 `isos start`，旧 `--config` 和 `--no-tui` 返回迁移提示；独立 `bin/egia --island-config <path>` 和 `islandctl --config <path> validate` 使用相同共享根。
- [ ] `etc/egia/modes.toml` 的提交预设仅使用 loopback，不含部署专用 gateway endpoint。
- [x] 浏览器 Origin 限制为 `https://www.os-island.com`，非浏览器客户端可不发送 Origin。
