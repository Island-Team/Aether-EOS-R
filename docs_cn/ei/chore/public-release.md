# 公开发布流程

[English](../../../docs/ei/chore/public-release.md) · [发布检查表](release.md) · [EOS 兼容性](../../island/eos-contracts.md)

计划中的首个公开版本为 v0.1.0，与 Python 包和 CMake 项目版本一致。所有门禁通过前不得发布。本开发分支将 EGIA 迁移到 EOS 2 提交 c43e5a32eafd9bf22b2dba2940662dcded60111c。它不是公开 EOS release tag；此兼容性迁移不会创建 Aether-EGIA-R release 或 tag。

## 兼容门禁

- 固定 EOS protobuf 与 action schema 的精确版本，并从该来源重新生成检入的 binding。
- 保留 EGIA safety admission、精确 capability join、持久 operation ID、终态 acknowledgement 校验、显式取消，以及丢失结果或 OUTCOME_UNKNOWN 时不重试的规则。
- 保持英文和中文 operation 线协议示例一致，并同步更新发布检查表。
- 通过仓库要求的 lint、type、import boundary、schema 和完整测试套件。
- 针对某个精确公开 EOS release 运行真实进程集成测试。当前开发 pin 本身不构成发布资格。
- 验证 Python 3.12 能通过文档说明的可选 interpreter、Conda 和 uv 环境运行；不得绑定到某个系统 Python 安装。

## 源码快照发布步骤

所有门禁通过后，从干净 checkout 创建公开发布并记录源码提交：

1. 运行发布检查表，确认 package、CMake 和文档版本一致。
2. 构建并检查源码包和 wheel；确认包含生成的 protobuf 模块与 EGIA 自有 northbound schema，且未重复打包 EOS schema。
3. 在记录全部兼容性证据后，创建经过审查的版本提交和 tag。
4. 在干净 Python 3.12 环境中从 tag 重建并验证安装、配置校验和文档中的启动流程。

不要为了勾选检查表而发布未完成资格验证的开发快照。
