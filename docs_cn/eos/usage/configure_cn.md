# 本地配置与 TOML 操作

[使用 EOS](README_cn.md) → **配置** → [完整配置键参考](configuration-reference_cn.md)

本页描述可选 C++ 侧链的 `eos.toml` 配置、原生技能和工作流。默认 Rust v2 路径使用通过 IsOS 应用的[节点图及实例档案](../interfaces/hardware_cn.md)；独立模型服务有单独的 IsOS 服务文件。C++ 侧链使用进程 TOML 选择一个硬件绑定，现场地址、设备身份、标定和端点保存在本地副本或独立部署目录。

## 创建本地文件

没有硬件时先选择 demo：

```sh
cp etc/eos/eos.demo.toml etc/eos/eos.toml
```

它选择 `backends/demo.toml` 的 `demo` profile；可选模型端点只是占位。实体设备可从示例开始：

```sh
cp etc/ext/live/binding.toml.example etc/ext/live/binding.toml
cp etc/eos/eos.toml.example etc/eos/eos.toml
```

示例仅包含通过 `aplay` 输出的主机扬声器。设置唯一 `robot_id` 与 `robot_name`，核实 ALSA 路由，只声明真实设备支持的 primitive 和已验证的限制。需要模型的 primitive 还应配置并单独启动对应模型服务。

## 选择一个 profile

`eos.toml` 必须包含且只包含一个绑定：

```toml
[mode]
[[mode.binding]]
backend = "backends/demo.toml"
profile = "demo"
```

`backend` 路径相对于 etc 根目录，指定文件中必须存在该 profile。profile 描述机器人身份、设备、primitive、调用限值、后端与 feed rate；设备 ID 由 EOS 生成，不能手工设置。用 `--etc` 或 `EOS_ETC` 指定配置根目录，`--config` 指定进程 TOML。生产环境的外部 root 使用绝对路径。

| 文件或配置表 | 作用 |
| --- | --- |
| `eos.toml` 的 `[gateway]`、`[log]` | gRPC listener 与日志设置。 |
| `eos.toml` 的 `[[perception.model]]` | 独立模型服务的出站客户端；EOS 不启动模型。 |
| `etc/runtime/stack.toml` 和外部 `module.toml` 的 `[[service]]` | 托管服务和持久驱动，见[启动说明](../debug/launch_cn.md)。 |
| `etc/ext/north-http/config.toml` 的 `[north_http]` | 独立 REST/SSE listener 及 EOS upstream。 |
| `etc/ext/north-http/config.toml` 的 `[action_list_audit]` | 外部非权威 ActionList audit。 |

## 添加本地技能和工作流

```sh
cp -a examples/skills/local-native share/eos/skills/native
cp examples/workflows/hello.json.example share/eos/workflows/hello.json
```

若 `share/eos/skills/native/` 已存在，保留文件并手工合并示例的 `CMakeLists.txt`、`skills.h`、`skills.cpp`。`example.hello@1.0.0` 不访问硬件，返回 `{"ok":true}`。新技能在本地 CMake 中加入源码并通过 `RegisterSkills` 注册；修改 C++ 后重新配置与构建。工作流示例调用此技能，启动时 EOS 读取 `share/eos/workflows/*.json`，格式错误会阻止启动。启动后可用 `workflow show hello` 和 `workflow run hello` 检查。独立动态库的 ABI 见[原生技能接口](../interfaces/skills/native/README_cn.md)。

检查这些文件受到忽略规则保护：

```sh
git check-ignore -v etc/eos/eos.toml etc/ext/live/binding.toml share/eos/skills/native/skills.cpp share/eos/workflows/hello.json
```

## 验证

启动 EOS 后检查严格解析错误，再启动 north HTTP 并查询 `/health/ready` 和 `/v1/eos/discover`。配置文件修改后需要重启进程；不要假设热重载。全部配置键和拒绝规则见[配置参考](configuration-reference_cn.md)，真实驱动准备见[后端指南](backends_cn.md)。
