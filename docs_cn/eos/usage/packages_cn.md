# 使用 island-pkg 管理 Island 扩展

[English](../../../docs/eos/usage/packages.md) · [软件包清单](../../island/interfaces/packages_cn.md) · [MuJoCo 设置](mujoco_cn.md)

新的 IsOS 部署使用 `isos packages` 管理软件包，通过部署图选择已安装版本。[IsOS 软件包指南](../../isos/usage/packages_cn.md)介绍搜索、安装、验证和删除。Python 组件默认使用 Python 3.12；服务启动命令可选择 uv、Conda 或明确的解释器，不依赖系统 Python。

本页其余内容介绍 EOS checkout 中的兼容软件包目录。IsOS 可用时，`island-pkg` 将 `search`、`info`、`install`、`list` 转交给它；`island-pkg list` 与 `island-pkg --list` 等价。旧版 `--root`／`--registry` 覆盖参数和其他旧命令仍在本仓库本地运行。设置 `ISLAND_PKG_LEGACY=1` 可显式使用旧版目录。

## EOS 旧版软件包目录

```sh
./bin/island-pkg search NAME
./bin/island-pkg doctor NAME
./bin/island-pkg install NAME
./bin/island-pkg lock add NAME --version 1.2.3
./bin/island-pkg lock verify
```

安装不会启用模块、加载技能动态库或启动进程。软件包根目录中的 `isos.lock` 记录选中软件包的准确 ID、版本、类型、来源、目标平台、契约和已安装文件树的 SHA-256。`lock add NAME [--version VERSION]` 选择已安装版本；`lock remove NAME` 取消选择；`lock show` 显示锁文件；`lock verify` 核对文件。锁文件按软件包 ID 排序并原子写入。选中的版本不能删除。直接从互联网共享库地址安装而不经过已审核软件包的功能尚未实现。

共享原生技能要求 v3 清单、`eos_contract = "eos.skill.v2"`，以及至少一个普通 `*.skill.so` 文件。`install` 只把动态库存入软件包目录，不会复制到 `skills/current`。`skills path|sync` 仍供旧加载器使用，并非运行时管理器的选择机制。公开目录命令拒绝私有后端发布。`doctor` 会指出不兼容目标和缺失库；MuJoCo 软件包还需要匹配的独立 MuJoCo 安装和 `MUJOCO_ROOT`。

## 配置外部模块

v3 软件包可包含激活模板。运行 `./bin/island-pkg activate NAME [--version VERSION]` 会将模板渲染到软件包目录中的隔离位置。必需的 `module.toml` 位于 `etc/ext/<namespace>--<name>/`，声明软件包身份、服务和可选的 EOS 绑定。检查该文件，使用 `lock add` 选择准确版本，再将其路径显式加入部署配置 `etc/runtime/stack.toml` 的 `modules` 列表。运行时管理器验证并应用选中的关系图；`island-pkg activate` 不修改运行中的栈。v1/v2 软件包仍可安装，但旧式激活布局会收到迁移错误。`island/hello` 示例无需 EOS 配置或激活。

```toml
version = 1
eos_config = "../eos/eos.toml"
modules = ["../ext/vendor--driver/module.toml"]
```

模块描述文件使用 `version = 1` 和含 `id`、`enabled`、`package`、`version` 的 `[module]`，可声明 `[[service]]` 和 `[[binding]]`。绑定指定后端、配置档和所需服务。外部配置归 `etc/ext/`，EOS 自有配置归 `etc/eos/`。参见[清单示例](../../island/interfaces/packages_cn.md#私有后端源码包)和[配置指南](configure_cn.md)。

## 构建与删除

`./bin/island-pkg build SOURCE` 构建并安装 v3 CMake 软件包。`--cmake-option NAME=VALUE` 覆盖选项；`--cmake-source DIR` 指定另一个 CMake 项目。私有后端包须位于本 checkout 外，可安装桥接程序和声明服务、绑定的 `module.toml` 模板。CMake 以当前用户身份运行，构建前应审查源码。构建完成后按上面的步骤使用 `lock add` 和适用的 `activate`。

`info NAME [--version VERSION]` 查看目录发布信息；`list` 查看已安装版本；`remove NAME [--version VERSION]` 删除版本。如果 `isos.lock` 仍选择该版本，或生成的激活配置仍引用它，删除会被拒绝。全局选项 `--root`、`--registry` 须写在子命令前。`island-pkg` 负责文件和配置生成，EOS 运行时管理器准入绑定，`island-launch` 管理所声明的进程。

开发者在目录页面提交源码仓库和 GitHub 发布链接。审核员检查资源文件、许可、平台验证和 EOS 集成后发布不可变清单。
