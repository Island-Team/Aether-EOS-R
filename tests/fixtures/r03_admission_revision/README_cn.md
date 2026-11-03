# R03 准入夹具

**状态：** R03 通过经评审的 EOS 与 IsOS 所有者合并及合并后全新主目录重放；本夹具仍是 EOS 聚焦证据。R05/R06 与 M3 总验收另行进行。参见[首次使用证据](../../../docs_cn/eos/develop/admission-revision_cn.md#eos-主题证据与-r03-验收)。
[English](README.md) · [gRPC 所有者契约](../../../docs_cn/eos/interfaces/grpc_cn.md#r03-执行器受治理准入目标契约) · [硬件边界](../../../docs_cn/eos/interfaces/hardware_cn.md#r03-dispatcher-准入目标契约) · [设计](../../../docs_cn/eos/design/runtime/admission-revision_cn.md)

**状态：设计用例已冻结；R03 已验收。** `cases.json` 保留 25 个编号，覆盖受管理修订来源、角色授权、精确 binding、假时钟新鲜度与期限、持久重放、停止恢复、操作所有权及当前各私有 socket 绕过入口。每行标明最低证据类别。[首次使用证据](../../../docs_cn/eos/develop/admission-revision_cn.md#eos-主题证据与-r03-验收)记录用例到测试映射、精确命令与所有者日志；修改预期结果仍须更新所有者契约并再次独立设计评审。运行 `python3 tests/fixtures/r03_admission_revision/check_cases.py` 校验表格与契约锚点。经评审的 EOS 与 IsOS 实现已经合并，合并后全新主目录 mock 底盘重放验收授权签发、启动交付及受治理的 gRPC 行为；输出位于 IsOS 主检出目录的 `logs/test/20261102-r03-cross-repo/post-merge-primary.log`。R05/R06 仍负责 MuJoCo 物理隔离与停止/回收证明。
