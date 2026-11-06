# Q03 IsosUI 所有者证据

[English](../../docs/isosui/q03-evidence.md) · [设计](q03-qualification_cn.md)

**状态：**已审查的 IsosUI 代码主题与隔离浏览器基准在选定源码 90579a9 上通过。封存的 Q03 final-17 选定重放在该源码上通过，Q04 final-5 在合并后的 IsosUI master 上通过独立归档审查。IsosUI [`v0.1.0`](interfaces/releases_cn.md) 已发布，Island-web 已部署固定摘要的托管归档。[已批准的发布资产 run-7](https://github.com/Island-Team/Island-eos/blob/refactor%28v2%29/runtime-main/docs/island/q04-merged-qualification.md)已完成既定范围的 Q04/M5 跨所有者发布关卡；公网 HTTPS 浏览器首次使用仍待验证。先前未完成的浏览器尝试保留为诊断证据。

## 源码与环境

- 选定代码及构建包源码：IsosUI 90579a9e86033f4180d81d93a212e7b8a61c1850，基于已获取的 master 920995788d8e29dd6ab594d31278033bb791cfeb。代码主题随后以 squash 方式合并为 master 4c745bf92f06cea3e2b10f5038817e8efba8ca06。Q03 证据文档当时基于该 master 提交；文档 HEAD 不产生新的构建身份。Q04 随后验证了下文记录的已合并 master 身份。
- 本地和托管 manifest 均声明 source_commit 90579a9e86033f4180d81d93a212e7b8a61c1850、版本 0.1.0、IsOS API schema 1 及各自目标。manifest SHA-256：本地 ef50ab9a7aef0a366a0e6946c2cec9e0ee4518b78e265abc753a27e7a42a129d；托管 d8f4d5615bc1eebcea9472abac4be5dbde7a7d4b579fed2099307e289cf4ba36。
- 夹具 manifest SHA-256：950b135196c6b7e54cb0fe3dcd5fe04e94117698ad187e3383ebfe3590c9ec08。锁文件 SHA-256：922c376224fb6814252b074f07e93b9b0d701e19fbafe262b9881f59992ac4e9。
- Linux 7.0.0-38-generic x86_64、12th Gen Intel Core i5-12490F、Firefox 157.0.1、geckodriver 0.37.1、Python 3.12.3、Selenium 4.38.0、Node 22.23.2、npm 10.9.9；浏览器视口 1280 × 800。原始基准文件保留精确的操作系统与 CPU 字符串。

## 复现与检查

在选定代码主题 90579a9 中，将 Node 22／npm 10 加入 PATH，并将 Selenium 4.38.0 加入 PYTHONPATH。完整输出及原始文件位于本地被 Git 忽略的 logs/test/20261103-q03-ui-process-reset/ 与 logs/test/20261103-q03-ui-90579a9/。

    npm run typecheck
    npm test
    npm run build
    node scripts/verify-bundles.mjs
    python3 -m unittest tests/q03_browser_benchmark_test.py -v
    python3 tests/fixtures/q03_browser_benchmark/v1/check.py
    git diff --check
    PYTHONPATH=/tmp/q03-ui-py python3 scripts/q03_browser_benchmark.py --target local --warmup 100 --samples 1000 --repetitions 3 --output logs/test/20261103-q03-ui-90579a9/browser-local-90579a9.json
    PYTHONPATH=/tmp/q03-ui-py python3 scripts/q03_browser_benchmark.py --target hosted --warmup 100 --samples 1000 --repetitions 3 --output logs/test/20261103-q03-ui-90579a9/browser-hosted-90579a9.json

TypeScript 通过；Vitest 的 41 个文件、191 项测试通过；两种生产构建及 manifest 校验通过；Python 基准运行器的 10 项定向测试通过；固定夹具检查与差异检查通过。本地和托管目标还各自通过了三次重复的小规模生命周期检查（每项一次预热、两次计时）：每次有独立的会话和驱动 PID，且均记录五项资源摘要。已审查的运行器改动关闭了两条证据丢失路径：浏览器启动或上一会话退出失败，以及完成用例后的资源校验失败，均在公布运行前写入失败检查点。

固定夹具包含 19 个图节点、10 条连线、一个活动底盘绑定、5 毫秒桩响应延迟和 2 秒交互期限。运行器在浏览器启动前独占隔离环回端口 16000、16001、16020。每次重复均使用新的 Firefox 与 geckodriver 会话，重新验证本地或托管构建包及已观测资源摘要。每个 Control 样本仅向 EOS 桩发送一次具有唯一 ID 的合成原语请求，并在同一运行、同一修订检查后显示匹配的终态。真实所有者或物理执行器没有收到这些请求。吞吐量以完成数除以浏览器单调时钟测得的区间之和；重置和预热墙钟时间另行记录。中位数用中间值，p99 用最近秩。

## 浏览器基线

两种完整运行器均以退出码 0 结束。每种目标分别完成三次重复，每次每项预热 100 次、测量 Graph 1,000 次和 Control 1,000 次：每种目标测量 6,000 次、失败零次。每次 Control 重复包含预热在内，均记录 1,100 个唯一合成 POST ID。每种目标使用三个不同的浏览器会话和 geckodriver PID，每次重复验证五项 JS 资源摘要，包括懒加载模块。原始文件保存每个样本的延迟、结果、响应字节数及 SHA-256。

| 目标 | 次数 | 用例 | 完成／失败 | 次／秒 | 中位数 ms | p99 ms |
| --- | ---: | --- | ---: | ---: | ---: | ---: |
| 本地 | 1 | Graph | 1,000／0 | 31.11 | 30 | 49 |
| 本地 | 1 | Control | 1,000／0 | 5.46 | 179 | 262 |
| 本地 | 2 | Graph | 1,000／0 | 32.07 | 29.5 | 48 |
| 本地 | 2 | Control | 1,000／0 | 5.15 | 180 | 280 |
| 本地 | 3 | Graph | 1,000／0 | 31.90 | 30 | 48 |
| 本地 | 3 | Control | 1,000／0 | 5.43 | 178 | 245 |
| 本地 | 合并 | Graph | 3,000／0 | 31.69 | 30 | 48 |
| 本地 | 合并 | Control | 3,000／0 | 5.35 | 179 | 280 |
| 托管 | 1 | Graph | 1,000／0 | 29.84 | 30 | 54 |
| 托管 | 1 | Control | 1,000／0 | 5.21 | 180 | 278 |
| 托管 | 2 | Graph | 1,000／0 | 27.38 | 32 | 60 |
| 托管 | 2 | Control | 1,000／0 | 4.27 | 230 | 281 |
| 托管 | 3 | Graph | 1,000／0 | 29.19 | 30 | 57 |
| 托管 | 3 | Control | 1,000／0 | 4.84 | 195 | 280 |
| 托管 | 合并 | Graph | 3,000／0 | 28.76 | 30 | 57 |
| 托管 | 合并 | Control | 3,000／0 | 4.74 | 227 | 280 |

原始 JSON SHA-256：本地 4960fc8535ecccbf9125603585668d4251a8d7a76080a062323eb4b4bac724de；托管 b7a092f27db974fad820e23a9b1a08c1ac0e5521e5b96e19e7c8ec8c251d3634。

此前 fecafe9 上共用长期浏览器的尝试虽然完成了一次本地完整重试，但托管重试在第三次重复的 Control 预热阶段、发送任何 EOS time 或原语 POST 前失败。另一次更早的本地尝试在第二次重复 Control 发送前失败。原始检查点和日志保留在被 Git 忽略的 logs/test/20261103-q03-ui-fecafe9/。这些未完成运行不计入本次精确源码基线，也不被视作成功。即使每次使用新会话，反复导航期间 Firefox 内存仍达到约 4–6 GiB；这是观察到的基准环境开销，不据此宣称性能目标。

## 已安装选定回放

封存文件位于 EOS 本地被 Git 忽略的 `logs/test/20261103-q03-selected-final-15/live/`。冻结选择的 SHA-256 为 `116aef2846750a96275645e4dec3021ad716ce6fb80b05c087a2ce377a74b990`；外置冻结构建回执为 `977293e097779f636cb70bde9f3d5657bc9351b0b9f67534ca7beb41a97ef915`；用例构建回执为 `20b2dc444d4f90ca1e4cf4b8a2404893c53cd20de19a090ce02debf0c539ab5d`。归档验证器对相机、G06/R03、P01/P03/U01、P07/U03、R06/A03 和显式迁移报告 `Q01_FULL_VERIFIED`。P01/P03/U01 包含幽灵设备负面路径：Apply 被拒绝后，图字节保持不变。

在可见的选定本地 UI 中，一个活动 `base.velocity@1.0.0` 绑定以 `x_m_per_s: 0.1` 和 250 毫秒租期发送命令。浏览器对命令 `ui-1fb70aba-7a11-4ca7-9b7e-738093ee07b3` 先显示 Sending、再显示 Completed；EOS 公共 bridge 授权结果读取对同一 ID 和活动修订 `4460cbb317cbaf76394936d6dc790db88466129b31a2bb1c519f4e2ce3c944e4` 返回 HTTP 200 与 Completed。选定的 Vision 面板渲染了 640 × 480 JPEG，验证后 SHA-256 为 `f3ff9f047ca4b88a0ad97d79f6097e68f17ca62a6dc160efdb1337a28961ce2b`；归档记录了对应的运行、门、来源、流和修订证明。所有者故障／重启与安全 Stop 由跨仓库回放验证，不属于上面的隔离浏览器基准。

托管路径检查在环回上使用**模拟的托管 Origin**，并将其转发为获准的 `https://os-island.com` Origin。直接使用合成环回 Origin 时，IsOS 和 EOS 均返回 403，且没有 `Access-Control-Allow-Origin` 头。这证明选定托管路径契约，不代表已完成公网 HTTPS 部署。在 final-15 关卡时，Q04 合并后提交的重放尚未执行。

## 最终选定与合并后提交的重放，2026-11-04

后续封存的 Q03 `20261103-q03-selected-final-17` 归档仍固定 IsosUI 选定源码 `90579a9e86033f4180d81d93a212e7b8a61c1850`，冻结选择清单 SHA-256 为 `35dd4e5aa4c209bf84935002a4eded756f3eb5a1c147b2fb12bd5d894e46b78a`。精确选定的 EOS 测试工具提交 `219f031bae9dbfc9d9f53f62c910b9e235bb2138` 对六组选定案例返回 `Q01_FULL_VERIFIED`。独立审核输出位于 EOS `logs/test/20261104-q03-final17-independent-audit/`；从较新的 EOS 检出重跑因测试工具摘要变化被拒，因此 Q03 采用选定源码上的验证结果。上文 final-15 观察仍作为历史记录，保留原始命令、修订及图像摘要。

封存的 Q04 `20261103-q04-merged-home-final-5` 归档固定 IsosUI `master` 提交 `f0d29848b57ba34e2557cc978fb294912064cc5c` 和 EOS 集成分支 `refactor(v2)/runtime-main` 提交 `0bd28b4d2c3ba90a43b81c02ea4e883ef5043db0`；其外部选择清单 SHA-256 为 `7f5cefa5b6521d42d28b88c916679f2b668ba93fd61bbcbbfed4bb8fcc722b87`。真实可见浏览器中的 Graph 编辑器显示不可用的 `ghost-device`，并禁用 Save and Run；IsOS 归属方响应指出 `mujoco_device_unavailable`。可见底盘控制以 `x_m_per_s: 0.1`、250 毫秒租期发送命令，并在 Sending 到 Completed 全程保留命令 ID `ui-cd5f2d76-f184-4fd4-84ce-1f4a31696397`。公共桥接的授权结果读取针对同一 ID 和活动修订 `351de2c95db402deb772809ec8f9032e37dfdcf2fab13b72f96deea4c8ed3069` 返回 HTTP 200 Completed。Vision 渲染了 640 × 480 JPEG，摘要 `2415a3115891e19200164708c64f45cedada33b5bddc4e18eb3f3fb0c7d1c7ce`，来源 `eos/hardware/camera/front`，门 `eos/interface/gate/front-preview`，代次为 95。归档保留真实截图及归属方 HTTP／事件回执。选定验证器返回 `Q01_FULL_VERIFIED`；独立归档评审检查了 3,630 个封存文件，没有 P1/P2 发现。此重放在环回地址模拟托管 Origin。后续 IsosUI 发布和 Island-web 部署见[发布契约](interfaces/releases_cn.md)；后续[已批准的发布资产 run-7](https://github.com/Island-Team/Island-eos/blob/refactor%28v2%29/runtime-main/docs/island/q04-merged-qualification.md)已完成既定范围的 Q04/M5 跨所有者发布关卡；公网 HTTPS 首次使用仍待验证。
