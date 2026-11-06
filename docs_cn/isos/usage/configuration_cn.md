# 配置 IsOS

本页面向选择本地或生产设置的操作人员。IsOS 先读取选定的 TOML 文件，再用 [`config/isos.example.toml`](../../../crates/isos/config/isos.example.toml) 中的完整默认值补充缺失字段。TOML 的 `#` 注释只是说明，不会成为设置。默认选定文件是 [`config/isos.toml`](../../../crates/isos/config/isos.toml)；`ISOS_CONFIG` 可用绝对路径指定另一个文件。选定文件只需包含要改变的值。

源码运行时的优先级为 `.env`、[`.env.example`](../../../crates/isos/.env.example)、选定 TOML、TOML 示例默认值。源码启动脚本先加载 `.env`，再从 `.env.example` 补充缺失的环境值；这些值会覆盖 TOML。安装后的二进制文件按已设置的进程环境变量、仅所有者可读写的 `~/.config/isos/active.env`、选定 TOML、示例默认值的顺序选择设置，不加载两个源码启动文件。TOML 中的相对路径以 IsOS 源码目录为基准。`active.env` 必须使用绝对路径；提供 `--activate-env` 时，安装器会以 `0600` 权限创建该文件。激活文件用于 IsOS 主目录、端口和归属方位置，不包含软件包特有设置。生产安装也自动使用相同默认值，并在选定 TOML 中保存本地覆盖值。文件不存在时，安装器会创建一个仅有注释的覆盖文件。

要修改端口，在选定 TOML 中添加 `[server]` 和 `http_port = 16000` 或 `ui_port = 16001`。两者都是 1024 至 65535 的整数 TCP 端口。要修改核心二进制目录，在 `[paths]` 中设置相应的 `aether_bin_dir` 或 `egia_bin_dir` 路径；示例列出了所有接受的设置、单位和默认值。不要把示例注释当作设置复制到环境文件。

如果已验证的软件包声明了必需的主机输入，在选定 TOML 中添加 `[package_host_inputs]`，并将声明的环境名称设置为绝对主机路径。每个键是最长 64 字符的大写标识符；`ISOS_`、`ISLAND_` 和 `EGIA_` 键保留。默认映射为空。IsOS 只向归属方传递软件包声明的输入，归属方检查路径的含义与就绪状态。同名进程环境变量优先。软件包特有路径值留在本地配置中，不放入共享示例。

如果 `isos` 报告配置错误，读取错误中的文件路径和行号或字段，修正文件后再次运行 `isos --help` 或 `isos status --json`。例如，`active.env` 的 `unsupported key MUJOCO_ROOT` 表示从 `~/.config/isos/active.env` 删除该键；软件包特有值属于软件包归属方。使用 `chmod 600 ~/.config/isos/active.env` 保持文件仅所有者可读写。TOML 格式错误会指出所选的 `ISOS_CONFIG` 文件。如果它指向不存在的文件，创建该文件或取消设置该变量。

[English](../../../docs/isos/usage/configuration.md)
