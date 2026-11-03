# R06 EOS 模拟器停止用例

[English](README.md) · [契约](../../../docs_cn/eos/interfaces/simulator-stop_cn.md) · [设计](../../../docs_cn/eos/design/runtime/simulator-stop_cn.md)

`cases.json` 固定了 59 项 EOS R06 所有者及跨所有者用例。凭据回归测试还断言：曾经就绪的子进程在停止响应丢失后仍保留已验证的 generation，`stop_epoch` 为 null，结果保持 unknown。`python3 tests/fixtures/simulator_stop/check.py` 仅检查清单、双语凭据示例及规范后端身份，不执行物理停止，也不能证明安全停止。实现测试须把每项用例绑定到单元、替身或真实子进程断言，并将完整命令输出保存至 `logs/test/<run-id>/`。
