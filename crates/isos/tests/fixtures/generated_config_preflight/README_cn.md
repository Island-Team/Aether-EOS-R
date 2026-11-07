# P04 生成配置夹具

[IsOS 配置契约](../../../../../docs_cn/isos/interfaces/nodes/instance_cn.md)归属此 schema。`cases.json` 冻结 P04 实现测试的精确用例 ID、操作、变更、诊断码、阶段、JSON 指针及事务恢复状态。普通准入失败的 `active_unchanged_on_failure` 要求已保存图字节、保存修订、活动修订、该修订选中的生成版本和子进程 PID 均不变。重命名后 `generated_commit_uncertain` 用例改为断言观察到的新旧修订／生成版本均完整、活动子进程 PID 不变，调用方须重新读取。P01 的 `package_changed` 保留 P01 诊断。

使用全新的 `ISOS_HOME`、EOS setup 桩或已构建的 `aether-profile`，以及返回三个托管覆盖文件的 EGIA schema-1 setup 桩。建立私有已安装 EGIA 组件与工厂默认目录；组件目录包含列出的八个有效文件和一个额外的嵌套普通文件。通过已配置的 EGIA 发行版选择两者，不写入任何源码仓库。桩只在渲染时报告 EOS gRPC 网关端点；R02 前保持 EGIA 运行时禁用。计算精确图字节摘要，记录选中的生成版本和清单。重复 Apply 证明逐字节幂等。修改一个所选程序或默认文件，以相同保存修订 Apply，证明生成新的不可变版本。Run／Restart 反面用例从运行中的仅 EOS 部署开始，以证明失败时不停止或替换。

缺失文件用例覆盖 EGIA 直接加载器（`prompts.toml`、`missions.toml`）、安全闸和适配器生成的 `profiles/isos.toml`。已安装默认文件符号链接、逃逸的覆盖路径、归属方拒绝、软件包收据变更、生成文件篡改、文件与清单成对篡改、过期选择记录以及缺失的子进程专属密钥均须在 spawn 或替换活动进程前失败，并使用冻结诊断。`missing_child_secret` 与 `later_child_missing_secret` 精确指向 `/cores/isos/layers/process/groups/component/nodes/egia/settings/env_from/EGIA_API_KEY`；后者证明停止 EOS 前先检查所有计划启动的子进程，而值只在 spawn 时交给 EGIA。成功 P04 Apply 后删除选择文件，独立必需标记仍应阻止 Run／Restart，即使旧 G03 目录还在。对首次／新图、变更图和同修订 Apply，在选择映射、必需标记和图重命名边界注入进程终止及 fsync 失败。恢复时按**实际观察到的**保存字节修订选择，验证保留条目／标记／目录，不用未来条目启动旧图；首次同修订 G03 升级中断后若 v2 条目有效，Run 前补齐标记。旧版用例确认安全的 `1.0.0` 图保留平面快照与原始字节。断言目录 `0700`、文件 `0600`、无未列出文件、排序清单、稳定生成摘要、图／清单／日志中无密钥值及 CLI／API 脱敏。实际读取器功能集合协商与升级建议属于 P05 夹具。
