# 输出选择

本设计解释 IsOS 只选择展示元数据、不充当数据生产者的原因。[数据接口](../interfaces/ui/README_cn.md)定义输出类型和选择；[HTTP 展示路由](../develop/http/presentations_cn.md)定义当前响应。

注册的输出端口及其 `DataType` 标识客户端可能显示的内容。归属生产者提供的描述符还应绑定表示形式、交付方式、端点标识、运行 ID 和图修订。生产者拥有负载字节及端点授权。IsOS 不会从软件包 ID 或类型名称推断图像、深度、检测或音频语义。

当前路由拒绝畸形或过期请求；在没有注册已验证的公开归属方描述符时返回不可用。它不伪造端点。归属方描述符集成是[未完成门槛](../maintenance/todo_cn.md)。

[English](../../../docs/isos/design/data.md)
