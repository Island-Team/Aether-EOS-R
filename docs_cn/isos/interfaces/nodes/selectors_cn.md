# 设置与选择器

本页供类型发布者和 UI 作者使用。IsOS 当前依据注册类型验证实例设置，并从声明的来源投影选择器选项。选择器是设置声明，不是软件包专属表单或请求时的驱动参数。[资源](../packages/resources_cn.md)解释经过验证的软件包选项。

| 字段 | 类型／单位 | 含义与验证 | 缺失／默认值 |
| --- | --- | --- | --- |
| `name` | 非空字符串／设置 ID | 在一个注册类型内唯一。 | 必填。 |
| `data_type` | 枚举／JSON 值类别 | `string`、`number`、`integer`、`boolean`、`array`、`object`；当前别名还包括 `string_array`、`string_table`、`positive_integer`、`nonnegative_integer`。 | 必填。 |
| `required` | 布尔值／存在规则 | 必填实例值缺失时，需要类型声明的默认值。 | 必填。 |
| `description` | 非空字符串／展示文字 | 告诉作者选择什么及其单位。 | 必填。 |
| `unit` | 非空字符串／物理或展示单位 | 说明数值或文本如何度量。 | 可选。 |
| `min`、`max` | 有限数字／设置单位 | 限定数值实例；`min` 不得大于 `max`。 | 可选；无对应边界。 |
| `default` | 与 `data_type` 匹配的值 | 填补省略的实例值。 | 可选；无默认值。 |
| `choices` | 非空匹配值数组 | 限定合法选项。 | 可选；无有限列表。 |
| `selector.provider` | 非空字符串／选项提供者 | 指定 `package`、`graph` 或本地 `ui` 选项来源。UI 提供者在浏览器中提供选项，不需要服务端选项组。 | 存在 `selector` 时必填。 |
| `selector.resource` | 非空字符串／资源类别 | 指定请求的选项类型。 | 存在 `selector` 时必填。 |
| `selector.package_from` | 非空字符串／设置引用 | 选择提供选项的精确软件包锁定。 | 可选。 |
| `selector.filter` | 对象／提供者过滤条件 | 不解释归属方负载而限制选项。 | 可选；空过滤条件。 |

IsosUI 根据声明渲染提供者选项。对于 `{"provider":"ui","resource":"keys"}`，它在本地捕获有界的 `KeyboardEvent.code`；对于图和软件包选择器，IsOS 提供可用 ID 与原因。键盘节点选择按键或组合键；IsOS 网关选择目标绑定和固定运动值。事件请求不能覆盖这些设置。预览选项不更改图。

[English](../../../../docs/isos/interfaces/nodes/selectors.md)
