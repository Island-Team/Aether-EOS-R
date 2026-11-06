# 旧实现调用者核查（2026-11-04）

[English](../../../docs/ei/chore/legacy-caller-audit.md)

本核查区分无调用者的旧运行时实现与仍被使用的兼容代码；不修改 EOS、IsOS 或 MuJoCo 线协议。

| 候选项 | 调用者证据 | 结论 |
|---|---|---|
| `src/egia/contracts/perception/body/vision.py` | 从当前 world perception 契约重新导出 `VisionObjectDetection` 和 `VisionSnapshot`；该文件随 `src/egia` 打包，提供 Python 导入兼容性。 | 保留，待单独的版本化 API 退役。 |
| `ISLAND_BUILD_ECOS` CMake 选项 | `tests/system/test_install_layout.py` 显式配置该选项；CMake 仍接受这个已弃用的构建选项。 | 保留及其测试。 |
| `src/island/boot.py` | `islandctl` 验证和 `tests/system/test_product_launcher.py` 使用其组合与路径检查；公开的 `bin/island` 已转发至 `isos start`。 | 保留验证实现，说明启动器区别。 |
| `island.eos.v1` 生成的 protobuf 绑定 | EGIA 仍使用 Gateway discovery/resources 和 SkillService；primitive 执行使用 `island.eos.v2.CapabilityService`。 | 保留已发布的线协议身份。 |

当前启动器拒绝 `bin/island --config`。独立 EGIA 使用 `bin/egia --island-config <path>`，IsOS 部署使用已应用的图。中英文布局指南已同步。

## 当前基线

- 命令：`bin/island` 转发至 `isos start`；`bin/egia` 独立启动运行时；`bin/islandctl validate` 检查旧产品布局。来源是启动脚本和 `src/island/ctl.py`。
- 安装：`CMakeLists.txt` 安装 `egia`、`egia-config`、现有兼容启动器、配置 TOML、共享资源、工厂默认值、`island_admin` 与 Markdown 文档；`egia_ext` 需显式启用。EGIA Python 运行时由 `egia-dist` 构建 wheel。
- 路由：`src/egia/io/northbound.py` 定义 `/api/aether/egia/health`、backend、chat、missions、review、context、model I/O、primitives、skills、perception 与 prompt 路由；契约见 `docs/ei/northbound.md`。
- 图版本：EGIA reader feature fixture 声明支持 `0.1.0` 和 `1.0.0`。IsOS 持有图持久化与迁移；EGIA reader 不得重写已保存的图。
