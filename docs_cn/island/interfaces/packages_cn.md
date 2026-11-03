# Island 软件包清单

[English](../../../docs/island/interfaces/packages.md) · [安装软件包](../../eos/usage/packages_cn.md)

Island-port 通过 `/api/packages/v1` 发布已审核的共享技能和扩展清单。本地客户端 `island-pkg` 在解压前核对归档大小及 SHA-256。后端包是 Island checkout 外的私有本地源码包，不从公开目录安装。清单独立版本化：v1/v2 发布仍可安装，但旧式激活布局会被拒绝；v3 定义原生技能、本地构建和外部模块布局。

`id` 是小写的 `namespace/name`，`version` 是 SemVer。`kind` 可以是 EOS 集成用的 `extension`、仅安装的 `example`、原生技能动态库的 `skill`，或私有桥接进程的 `backend`。公开资源声明 HTTPS 归档、SHA-256、字节数、`os`、`arch`、最低 glibc、`eos_contract` 和所需共享库。共享原生技能使用 v3 和 `eos_contract = "eos.skill.v2"`，归档必须至少包含一个普通 `*.skill.so` 文件。`example` 的契约为空且无需激活。客户端只选择兼容目标；目前没有依赖求解器或安装 hook。

EOS 集成的 v3 `activation.files` 将软件包内的 TOML 模板映射到 `etc/ext/<namespace>--<name>/`。其中必须有 `module.toml`，包含 `version = 1`、`[module]` 身份及发布字段，并可声明 `[[service]]` 与 `[[binding]]`。`id`、`package`、`version` 必须与已安装发布匹配。`{{package_root}}` 展开为安装目录，`{{package_file:relative/path}}` 展开为已安装的普通文件。激活只生成隔离的外部配置；不会选择发布、启用模块或启动进程。检查后，须将模块路径显式加入 `etc/runtime/stack.toml`。

`island-pkg lock add NAME [--version VERSION]` 在用户软件包根目录的 `isos.lock` 中选择准确的已安装发布；`lock remove NAME` 取消选择，`lock verify` 按已安装文件树的摘要验证。条目记录软件包 ID、版本、类型、来源、文件树 SHA-256、目标平台和契约。锁文件按 ID 排序并原子写入。安装和构建不会修改它；选中的发布不能删除。未来可能支持以互联网共享库地址作为软件包来源，目前并未实现。

归档条目只能是归档根目录下的普通文件或目录；符号链接、路径穿越、特殊文件及超限内容会被拒绝。审核通过的发布版本不可修改。

## 私有后端源码包

`island-pkg build SOURCE` 读取 `SOURCE/island-package.toml`，用 CMake 构建并安装到用户本地的版本目录。后端的 `SOURCE` 必须位于 Island checkout 外。项目与清单不在同一目录时用 `--cmake-source DIR`；`target` 和 `install_component` 限定桥接目标。例如 `/path/to/private-ros2-backend/island-package.toml`：

```toml
schema_version = 3
id = "private/ros2-bridge"
kind = "backend"
version = "0.1.0"

[source_build]
system = "cmake"
target = "island_pkg_ros2_backend"
install_component = "island_pkg_ros2_backend"
resources = ["config/module.toml"]

[source_build.cmake_options]
EOS_BACKEND_ROS2 = true

[[activation.files]]
source = "config/module.toml"
destination = "module.toml"
```

私有 `config/module.toml` 声明桥接进程、相关驱动和 EOS 绑定：

```toml
version = 1

[module]
id = "private--ros2-bridge"
enabled = true
package = "private/ros2-bridge"
version = "0.1.0"

[[service]]
name = "ros2-bridge"
role = "managed"
command = [{{package_file:bin/eos-ros2-bridge}}, "--binding", "{ros2_binding}", "--profile", "{ros2_profile}", "--socket", "{ros2_bridge_socket}"]
ready_unix = "{ros2_bridge_socket}"
timeout_ms = 30000

[[binding]]
backend = "backends/ros2.toml"
profile = "ros2"
requires = ["ros2-bridge"]
eos_after = ["ros2-bridge"]
```

将外部驱动声明为 `role = "driver"` 服务，并放入桥接服务的 `after` 依赖列表。加载 ROS 2 环境后，从 Island checkout 运行 `./bin/island-pkg build /path/to/private-ros2-backend --cmake-source .`。`resources` 在 CMake 安装后复制普通源码文件；激活生成新的配置目录。`island-launch` 管理声明的进程，EOS 通过适配器端点访问后端。本地技能源码包也可使用同一构建机制且无需激活。安装技能不会将动态库复制到加载目录，也不会执行它；运行时管理器准入选中的技能工作进程。CMake 源码及依赖以当前用户身份运行，构建前应审查。
