# 原生 skill 与共享 skill 边界

原生 skill 是由部署提供的 EOS 库，用于描述可复用的设备行为。EOS 加载其代码，EGIA 发现其元数据并将其作为规划指导。原生 skill 不是 JSON 工作流、EGIA 命令，也不能替代有类型 primitive。

每个 profile 在恰好一个配置共享根目录的逻辑 `skills/<skill-profile>/` 下拥有边界文档。检出默认值将 Island profile 解析到 `../share/skills/island/README.md`，安装后则解析到 `share/island/` 下。skill 实现与加载是 EOS 部署私有细节；本仓库只通过 `SkillService` 交互。

skill 元数据应提供稳定的 SemVer 名称/版本 identity、用途说明、自包含的 parameter/return JSON Schema、带精确版本的 primitive reference，以及 resource URI template。Native ABI 与库来源属于 EOS 私有部署信息。`SkillService.ListSkills` 返回完整内联 descriptor，`GetSkill` 解析一个精确 identity。有效空 catalog 受支持；临时不可用仅降级 skill view，缺失或格式错误的 service 则不兼容。EGIA 据此辅助规划；本阶段 runtime 仍只调用 primitive。operator 流程可组合受支持的 `arm.init` primitive 来请求部署配置的初始关节姿态，但该 primitive 对 planner 隐藏，并要求 `[primitives].expose_operator = true`。

参数校验和物理安全仍由 primitive 与 EOS 层负责。安装库后重启 EOS，确认 `ListSkills` 返回完整 descriptor 且每个精确 primitive name/version 都存在于 gateway discovery catalog，再将 skill 用于计划；新增 primitive 时应补充契约、发现、注册表、安全门和确认消息测试，并验证 `/api/aether/egia/skills` 的描述性 projection。
