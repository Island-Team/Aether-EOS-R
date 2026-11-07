# 控制授权边界

[English](../../../../docs/isos/interfaces/cores/control-grants.md) · [设计](../../design/server_cn.md)

已实现的 IsOS 授权包由每次运行的私有文件组成。两个控制授权文件是随机且互不相同的秘密，仅通过保留的所有者环境路径交付。私有 `verifier.json` 具有下列精确字段。IsOS 检查仅所有者可用的目录/文件权限、无符号链接祖先、选中运行与修订及授权摘要。停止和替换撤销这些文件。EOS 负责执行器命令准入；软件包贡献不能建立另一条路径。

| `verifier.json` 字段 | 类型 / 单位 | 用途与验证 | 缺省值 |
| --- | --- | --- | --- |
| `schema_version` | 整数 / 版本 | 当前私有验证文件值 `1`。 | 必填。 |
| `run_id` | 字符串 / 运行身份 | 精确授权包代次。 | 必填。 |
| `active_revision` | 小写 64 位十六进制 / SHA-256 | 绑定保存的图字节。 | 必填。 |
| `egia_control_sha256` | 小写 64 位十六进制 / 授权字节 | 第一份私有授权的摘要。 | 必填。 |
| `operator_control_sha256` | 小写 64 位十六进制 / 授权字节 | 第二份私有授权的摘要。 | 必填。 |

接收者、能力和到期字段**不在**此已实现验证文件中。重新设计的 `@v1` 授权封套需要所有者迁移和单独契约评审，目前未启用。

## 交付路径与失败代码

`EOS_CONTROL_GRANTS_FILE`、`EGIA_EOS_CONTROL_GRANT_FILE` 和 `AETHER_BRIDGE_CONTROL_GRANT_FILE` 是受保留的私有交付环境键；图参数不能覆盖它们。选中所有者的 `managed-control-grants` 能力在发放前检查。失败代码为 `control_grant_unsafe_path`、`control_grant_invalid_file`、`control_grant_mismatch`、`control_grant_issue_failed`、`control_grant_delivery_failed` 和 `control_grant_reserved_arg`。它们分别涵盖权限/路径、文件内容、运行与摘要不一致、发放、交付及保留参数冲突。所有失败都在启动或替换前关闭权限。
