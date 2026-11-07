# EOS 技能发现

[English](../../../docs/ei/southbound/skills.md) · [南向接口概览](../southbound.md)

完成必需的本体和资源发现后，EGIA 调用 `SkillService.ListSkills`。每个描述符以精确名称和版本为键，包含描述、参数与结果 Schema、精确原语依赖及可读资源模板。Native ABI、库、provider 和 build 信息属于 EOS 私有实现。

有效空 catalog 是受支持状态。临时 `UNAVAILABLE` 仅使 skill view degraded，不会使本体/资源代次失效。缺失 service、格式错误或重复的 descriptor、无效 JSON Schema，或 gateway discovery 中不存在的依赖会使 peer 不兼容并阻止该代次发布。提示词只显示其原语依赖在 EGIA 策略下可见的组合。

生成的传输绑定也包含异步执行 RPC。本集成阶段只使用 catalog discovery；受控执行将在后续阶段引入。
