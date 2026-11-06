# EOS 本体发现

[English](../../../docs/ei/southbound/body.md) · [南向接口概览](../southbound.md)

`Discover` 是唯一的 EOS 本体与原语绑定发现调用。EGIA 将其与 `ListResources` 及 `SkillService.ListSkills` 组合，校验完整响应，并原子替换一个已配置机器人别名的代次。

机器人描述符携带 `id`、`name`、`model`、`description`、运行 `state`、`health`、急停状态、可选电量百分比及机器人作用域原语绑定。`robot.stop`、`safety.estop` 等机器人作用域绑定使用空设备 ID 调用。

每个设备描述符携带 `id`、`class`、`part`、`description`、`state`、`health` 及设备作用域原语绑定。后端名称、驱动版本、端点、原始遥测、配置内部信息和媒体均不跨越该边界。

每个原语绑定提供精确名称与版本、描述、直接或高级类型、健康状态、支持的 `relative`/`absolute` 参考模式、可选绝对坐标系、强制数值限制、接受的媒体类型及依赖健康。EGIA 在诊断目录中保留所有有效绑定；只有健康为 `ready`、精确 Schema 已知且本地暴露策略允许的绑定才进入可调用注册表。

发现失败不会替换上一份完整代次。成功重新发现会一并移除已消失的设备、资源、技能和原语目标。监控器使用 `GetStatus.started_at` 作为 EOS 进程代次身份，并执行有界重新发现，避免健康状态和绑定长期陈旧。
