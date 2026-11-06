# Island-egia 仓库布局

[English](../../docs/island/layout.md) · [文档指南](../README.md) · [组合数据流](composition.md)

Island-egia 是 Python EGIA 运行时，通过 `island.eos.v1.GatewayService` 发现 EOS 资源，并通过 `island.eos.v2.CapabilityService` 执行 primitive operation；它不实现设备运行时。IsOS 负责部署组合，`bin/island` 与 `isOS-Aether.sh` 转发至 `isos start`。独立运行 EGIA 使用 `bin/egia` 或 `egia-launch.sh`。IsOS EGIA 节点使用 `--managed --no-tui`；受管理模式拒绝内部 sandbox。

## 目录与所有权

| 路径 | 当前用途 |
|---|---|
| `bin/` | 公开命令 `island`、`egia`、`islandctl` 和 `island-stop`。 |
| `etc/` | EGIA 和旧产品布局的配置、模型绑定、连接 profile、prompt 与安全策略。 |
| `libexec/egia_ext/` | 私有终端扩展；staged install 默认不包含它。 |
| `scripts/platform/` | ARM64 与 x86-64 checkout 构建和启动方法。 |
| `share/defaults/` | 打包的 Island 与 EGIA 工厂配置。 |
| `src/egia/` | EGIA 运行时、契约、核心管线与 I/O。 |
| `src/island/` | 产品配置、验证、路径解析及历史组合工具。 |
| `tests/` | 单元、集成、schema、启动器与扩展测试。 |
| `var/` | Git 忽略的状态、缓存、日志、运行记录与临时数据。 |
| `docs/`、`docs_cn/` | 英文与中文文档。 |

`build/` 和 `dist/` 是生成物，不是源码或运行时导入根。`etc/island.toml` 的默认共享资源根为 `share/` 与 `../share/`；同一相对资源路径出现在两个根时必须报错，不能使用覆盖优先级。EGIA 持有本地工厂默认值；EOS 持有共享线协议、primitive schema 和 skill 边界。

## 独立运行与 IsOS 部署

EI 组件使用统一仓库根目录的 EOS 契约和 `bin/eosd`。在 `ei/` 执行 `./bin/islandctl validate` 检查资源根和启动器。独立 EGIA 用 `./bin/egia --island-config <path>` 选择其他产品配置。`bin/island` 仅接受 IsOS 部署参数并转发到 `isos start`；旧的 `--config` 参数会被拒绝。受管理部署应将相同设置应用到 IsOS 图，而不是向 `bin/island` 传入旧 TOML。

公开启动器可从仓库外调用。不可变资源从配置的共享根读取；可变数据写入 `var/` 对应角色。`island.path.Layout` 也支持显式指定 portable 与 installed 布局。IsOS 根据已应用图决定组件顺序、就绪检查、进程所有权与停机。`island.boot` 仍供 `islandctl` 验证使用，但不是公开整栈启动入口。

`egia-build.sh` 和 `egia-launch.sh` 按 `uname -m` 选择 ARM64 或 x86-64 方法。构建方法用 `uv` 安装锁定依赖；架构启动方法在需要时引导 `.venv`，然后调用独立的 `bin/egia`。默认请求 Python 3.12；`EGIA_PYTHON` 选择已有解释器，`EGIA_PYTHON_VERSION` 更改 checkout 的 `uv` 请求版本。CMake 要求 Python 3.12 或以上，支持 `-DPython3_EXECUTABLE=<interpreter>`。

CMake staged install 复制配置的共享根，验证资源路径无冲突，然后生成 `share = ["share/island"]` 的安装版 `island.toml`。它还将 EGIA 工厂 TOML 安装到 `<prefix>/share/defaults/egia`，供 IsOS 系统模式使用。`-DEGIA_INSTALL_PRIVATE_EXTENSIONS=ON` 可在私有部署中安装 `egia_ext`；默认不安装。`ISLAND_BUILD_EOS` 是当前构建选项，`ISLAND_BUILD_ECOS` 是仍被测试使用的弃用别名。

用 `isos start` 启动已应用部署，用 `./bin/egia` 独立启动 EGIA，用 `./bin/islandctl validate` 检查旧产品布局。目录中不写入运行时状态；状态、缓存、日志和临时文件均写入对应的 `var/` 角色。
