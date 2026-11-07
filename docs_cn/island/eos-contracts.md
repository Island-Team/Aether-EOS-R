# EI–EOS 契约边界

[English](../../docs/island/eos-contracts.md) · [共用契约](contracts.md) · [EI EOS 接口](../ei/interfaces/eos.md)

EI（原 EGIA）和 EOS 虽属于唯一 IsOS 图核心，进程间仍保留显式传输边界。IsOS 提供绑定修订的投影。EI 通过已发布的 EOS gRPC 服务取得发现结果与准确能力绑定，并经 EOS 网关发送类型化操作。EOS 校验活动图、目标、负载与安全限值，再由 Dispatcher 和日志准入工作。EI 不打开 MuJoCo 套接字，也不直接调用适配器。

EOS 拥有 `share/eos/` 中的 protobuf 和 JSON Schema 编写源。`ei/` 中的 EI Python 生成绑定来自本地源文件。已发布的 `island.eos.v1` Gateway 和 `island.eos.v2` CapabilityService 身份独立于图 `0.2.0` 和源码 `0.2.1`。历史 `egia.contracts.*` 模式 ID 与旧源码固定版本是兼容及来源记录，不表示需要恢复同级仓库检出。

调用方生成的操作 ID 是持久幂等键。EI 跟踪待处理结果至所有者终态；传输丢失或 `OUTCOME_UNKNOWN` 不授权自动重发。取消使用已发布操作边界。准确参数、确认与故障码见 [EOS 接口](../eos/interfaces/README_cn.md)及 [EI EOS 契约](../ei/interfaces/eos.md)。契约变更须重新生成绑定，并由两个所有者测试准入、拒绝、取消与不确定结果。
