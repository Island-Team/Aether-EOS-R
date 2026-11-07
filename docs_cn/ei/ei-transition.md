# EI 命名迁移（0.2.1）

[English](../../docs/ei/ei-transition.md)

EI 表示 Embodied Intelligence，是 IsOS（Island OS）中的认知与决策运行时。EOS 表示 Embodied OS，继续负责具身执行和执行器准入。两者由同一个 IsOS 图管理，但运行进程保留各自的安全与生命周期边界。

新命令使用 `bin/ei` 和 `bin/ei-config`；Python 集成可以导入 `ei`。0.2.1 继续接受已发布的 `bin/egia`、`bin/egia-config`、`egia` Python 导入以及 `EGIA_PYTHON`、`EGIA_ETC`。同时设置新旧环境变量时，`EI_PYTHON` 和 `EI_ETC` 优先。过渡期间，`etc/egia/` 中的配置文件和现有线协议标识保持兼容。`ei` 与 `egia` 指向同一套实现模块，因此不要在同一 Python 进程中将两个导入路径当作独立单例使用。

图核心迁移与配置投影由 IsOS 负责。`egia-config` 适配器保留旧图读取能力；新图必须先由 IsOS 验证并投影，EI 才能启动。记录已发布合同或验证结果的历史文档和测试样例保留 EGIA 名称。

对 `0.2.0` 图，IsOS 将单个 `isos` 核心中的 `ei_*` 层和源图修订号投影给 EI。EI 只接受精确的 `isos.ei_<layer>.<name>@v0.2` 阶段类型，验证布局与进程内边，再渲染绑定修订号的内部 `stages.json`。未知类型、错误位置和无效边均返回带 JSON 指针的诊断。为兼容旧部署，保留 `egia` setup 角色与 `0.1.0`/`1.0.0` 读取器；在运行时导入路径更名之前，阶段文件仍使用内部 `egia` 路径。
