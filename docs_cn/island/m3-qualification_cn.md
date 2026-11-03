# M3 受治理执行验收

[English](../../docs/island/m3-qualification.md) · [里程碑](implementation-milestones_cn.md) · [详细 TODO](implementation-todo_cn.md) · [EOS 停止设计](../eos/design/runtime/simulator-stop_cn.md)

**状态：M3 按受治理执行的限定完成条件于 2026-11-02 验收。** R01–R07 均有经审查的所有者证据。精确合并版本的首次使用重放在 pick-place 与 navigation 场景运行真实 IsOS → EOS → MuJoCo 图；EGIA 类型化客户端发现活动底盘/机械臂绑定并在三个用例中提交六个关联操作。这关闭 M3 完成条件；M4 智能体/工作台阶段及 M5 产品发布全程仍未完成。

## 精确输入与保存的证据

| 所有者 | 源码提交 | 本次用途 |
| --- | --- | --- |
| EOS | `2437f81a661be57406029015b63530c8ed6b898a` | 重新构建 runtime、bridge、config server 与 profile。 |
| IsOS | `b5553ad9a25527f3481a4ff6e64aeb3b92c43497` | 重新构建管理器并运行全新主目录。 |
| MuJoCo | `4e6b5449ccd25be53ad95b13baa2c89f7ed91e10` | 重新构建所链接的 `island/mujoco-sim@0.1.1` 包。 |
| IsosUI | `4029728994d5a53dac76f2a566534e0472c4fab6` | 仅锁定源码以核查指南；本次使用已有本地 `dist/local`，未重新构建或实时验收 UI。 |

独立的 `reviewer-verdict.md` 支持原始 R06 条目勾选及限定范围 M3 关闭。EOS 主检出目录中被忽略的 `logs/test/20261102-m3-exact-merged-replay/` 含覆盖 **158 个证据文件**的校验和清单（`evidence.sha256`；连同清单共 159 个文件）。评审者最初审核 156 项清单；最终扩展清单校验 158/158 项通过。其中的 `qualification.md`、`commands.txt`、`source-pins.txt`、二进制哈希、事件顺序、各用例目录及完整命令日志构成可复现记录。重放最初发现验证脚本误认 P01 的 `local_link` 路径；修正后验证脚本及三个实时用例均通过，初次脚本错误回溯仍保留。主检出目录的 `temp_design.md` 未改动。

## 完成证据与边界

R01 的已合并路由实现和当前提交测试覆盖加权公平、频率上限、有界队列、溢出与停止栅栏。R02/R03 经审查的 gRPC 和 Dispatcher 记录覆盖类型化发现、完整代际就绪、角色/修订/绑定准入、假时钟新鲜度、持久终态/未知 journal 以及重启不重试。R04 经审查的宿主 ABI、IsOS 选择与可检查退休，加上当前提交负例测试，覆盖未配置及无效包在 guest 调用前拒绝，并覆盖未声明能力由 Broker 在 Dispatcher 或物理释放前拒绝。R05 经审查的严格套接字、绑定校验、去重和健康证明支持真实子进程。R07 单独封存的浏览器 bridge 与首次 UI 原语演示证据支持其所有者条目；本次未重放 UI 行为。[详细 TODO](implementation-todo_cn.md#验证记录)链接各所有者记录。

当前提交重放从全新主目录执行包链接、场景/节点选择、Validate 与带修订检查的 Apply、启动及权威就绪状态。EGIA 类型化发现取得 `base.velocity` 与 `arm.movej`；六个已准入操作 ID 均对应 EOS journal 中的 `Completed` 记录。pick-place 和 navigation 的选中原始 Stop 收据确认底盘与机械臂停止、准入关闭、后端进程组排空/回收及套接字移除。丢失伴随停止套接字得到 `eos_stop_unconfirmed` 和未知原始收据；管理器重启保留降级旧运行，并拒绝 Restart 与 Start，没有新部署或 journal 重放。当前提交的宿主技能、Dispatcher、路由、IsOS 选择/预检及夹具聚焦测试通过。封存的 R06 精确提交及合并输入负例覆盖子进程/提供者死亡、强制停止、无响应及错误管道收据；这些实现源码在当前合并提交无差异。这些实时负例没有全部在本次组合重放中重复执行。

R06 按**原始条目的所有者验收条件**勾选：关闭准入、有界物理停止通道、子进程终止/回收前确认或标记未知、逆序停止依赖进程，以及跨所有者证据中的指定故障/清理测试。更广泛的实时证明仍有限：物理部分停止确认、Stop 正在进行时管理器死亡，以及跨所有者的就绪后 GUI 渲染阻塞未端到端执行。本次也未验收完整 EGIA 阶段流水线、浏览器 UI、基准、升级/迁移或完整 M5 运行。M4 与 M5 仍未完成。
