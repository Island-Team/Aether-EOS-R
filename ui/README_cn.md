# IsOS 中的 IsosUI

[English](README.md) · [IsosUI 指南](../docs_cn/isosui/README_cn.md) · [IsOS 架构](../docs_cn/island/architecture_cn.md)

**尚未发布的 0.2.1 源码目标。** IsosUI 位于统一 IsOS 仓库的 `ui/`。IsOS（Island OS）拥有唯一图核心。EOS（Embodied OS）与 EI（Embodied Intelligence，原 EGIA）仍是图内分别受监管的进程；经过验证的 `island/mujoco-sim@0.1.1` 软件包仍为外部依赖。下方历史 IsosUI v0.1.0 发布与注明日期的测试结果只证明其原始输入；不存在 v0.2.1 标签或产品发布。

## 从统一源码构建

使用 Node.js 22 和 npm 10。从仓库根目录运行：

```bash
cd ui
npm ci --no-audit --no-fund
npm run typecheck
npm test
npm run build
```

`npm run dev` 与 `npm start` 在 `127.0.0.1:16001` 提供独立前端。浏览器使用 `http://127.0.0.1:16000/api/isos/v1` 的 IsOS 回环 API 和 `http://127.0.0.1:16020/api/eos/v2` 的 EOS 桥接；构建时可通过 `ISOSUI_API_ORIGIN` 或 `ISOSUI_EOS_API_ORIGIN` 选择其他 HTTP 回环端口。界面不发送 URL 令牌或 Authorization 请求头。IsOS 从经过验证的 `dist/local/` 产物提供已安装工作台；Island-web 在 `/isosui/` 托管经审核的 `dist/hosted/` 固定源码提交构建，并在 `/en/isos` 与 `/cn/isos` 展示。

| 构建 | 输出 | 资源前缀 |
| --- | --- | --- |
| `npm run build:local` | `dist/local/` | `/` |
| `npm run build:hosted` | `dist/hosted/` | `/isosui/` |

每种产物都包含 `index.html`、`assets/`、`IslandTeam.png` 和 `manifest.json`。清单记录包版本 `0.2.1`、准确源码提交、构建目标和独立维护的 IsOS API 模式版本 `0.1.0`。固定源码提交构建不创建发布版本。

## 图与工作台

IsosUI 只编辑含唯一 `isos` 核心的 `0.2.0` 图。管理器层保留原名；EOS 域使用 `eos_*` 层，EI 域使用 `ei_*` 层，浏览器节点使用 `ui`。注册类型 ID 使用 `isos.<layer>.<name>@v0.2`。IsOS 拥有绑定修订的已选目录，并校验每次 Apply 与 Run。已保存的 `0.1.0` 和安全旧版 `1.0.0` 图在 IsOS 显式迁移前只读；未知类型或未来图版本同样只读。画布布局单独保存在本地浏览器中，不写入配置图。

首页显示软件包凭据、就绪、保存与活动修订、日志和进程操作。图页面根据注册类型的准确位置编辑类型化设置与端口。EOS 展示活动图中的硬件观察与已准入原语控制；EI 展示进程状态和日志，包括历史 EGIA 进程 ID。浏览器按键输入位于 `ui` 节点。图连线不会发送命令。IsOS 拥有 Stop；EOS Dispatcher 拥有执行器准入。Vision 使用独立 EOS 回环 HTTP/WS 契约，只接受运行和修订匹配的证据。操作顺序与准确字段规则见[首次使用](../docs_cn/isosui/usage_cn.md)、[目录](../docs_cn/isosui/catalog/README_cn.md)和[接口](../docs_cn/isosui/README_cn.md)。

## 历史验证

下方注明日期的记录描述较早的独立 IsosUI v0.1.0 版本及其当时测试的伙伴版本，不构成统一产品 0.2.1 的资格验证。

独立前端已在 Node.js 22 环境下运行 `npm test`、`npm run build` 与 `npm start`；`127.0.0.1:16001/en/isos` 返回 HTTP 200，本地构建包含 `127.0.0.1:16000` 的 API 地址。完整输出保存在本地 `logs/test/20261030-npm-frontend/`。

节点选择与布局检查：`npm test`、`npm run build` 和 `npm start`（`ISOSUI_PORT=16041`）；开发服务器返回 HTTP 200，警告不再弹出遮挡层，编译错误仍会弹出。日志：`logs/test/20261030-graph-selection/`。

节点图模块按需加载为独立 JavaScript 块，Webpack 运行时代码也单独输出。自托管中文字体文件仍由浏览器按需请求；Webpack 的 JavaScript 体积警告不计入这些字体资源，但继续检查 JavaScript 块和首屏入口。构建结果记录在 `logs/test/20261030-webpack-size/`。

EOS 控制工作台在 Node.js 22 环境下通过 `npm test`（25 个测试）、`npm run build`（类型检查及本地/托管构建）、`git diff --check`，以及 `ISOSUI_PORT=16041 npm start` 后运行 `curl -fsSI http://127.0.0.1:16041/en/isos`（HTTP 200）。构建没有警告。完整输出保存在 `logs/test/20261030-eos-control/`。运行时控制调用需要已应用且正在运行的节点图及 EOS HTTP 桥；模拟和仿真命令测试位于 EOS 仓库。

适配器状态监视器更新通过 `npm test`（26 个测试）、`npm run build`（类型检查及本地、托管构建）和 `git diff --check`。两种构建均无警告；输出保存在 `logs/test/20261030-eos-monitor-status/`。EOS 桥通过 `status` 返回 `state` 与 `active_command_id`；执行器停止后仍可保持健康并提供最近的观测。只有状态为新的 `ready` 且健康为新的 `healthy` 时才显示实时。目标恢复实时前，原语调用不可用；面板提示重启 EOS 以重新启用已停止的目标。

软件包节点集界面检查：`npm test`（31 个测试）、`npm run build`（类型检查及本地、托管构建）和 `git diff --check` 均通过。完整输出保存在 `logs/test/20261030-package-node-sets/`。编辑器通过 IsOS 预览而不保存；服务器端的组合及验证需要兼容版本的 IsOS。

节点图工作台与视觉客户端检查：`npm test`（41 个测试）、`npm run build`（类型检查及本地、托管构建）和 `git diff --check` 均通过。完整输出保存在 `logs/test/20261030-graph-vision/`。二进制帧测试覆盖元数据、尺寸限制及无效 JPEG 的拒绝情况。端到端画面流仍需运行中的 EOS 相机网关和 IsOS 节点图修订。

视觉自动连接检查：`npm test`（53 个测试）、`npm run build`（类型检查和两种构建）及 `git diff --check` 均通过。完整输出保存在 `logs/test/20261031-vision-autoconnect/`。网关选择测试覆盖标准及展开的节点路径、用户选择和不存在的连接。此次 UI 检查期间 IsOS 与 EOS 的本机 API 均未运行，因此浏览器实时流仍需集成验证。

UI 选择器与构建验证（2026-10-31）：`npm test` 通过 45 项测试；`npm run build` 通过 TypeScript 检查、本地与托管构建；`git diff --check` 通过。完整本地日志保存在忽略提交的 `logs/test/20261031-build-catalog-visuals/{test,build}.log`。选择器及程序构建控件在当时需要配套的 IsOS 接口；接口不可用时，UI 会提示选项服务不可用，现有节点图字段仍可查看。

文档对齐检查（2026-10-31）：`npm test` 通过 49 项测试；`npm run build` 通过 TypeScript 检查，以及两种构建产物和清单检查；`git diff --check` 通过。完整输出位于被忽略提交的 `logs/test/20261031-docs-code-alignment/` 目录。测试输出包含 Sass 旧版 JavaScript API 的弃用警告。

节点图操作与画布稳定性检查（2026-10-31）：`npm test` 通过 52 项测试，`npm run build` 通过 TypeScript 及本地/托管构建检查，`git diff --check` 通过。完整输出保存在忽略提交的 `logs/test/20261031-graph-run-ui/`。此界面依赖对应 IsOS 更新中的构建状态及过期构建恢复 API。

可选核心就绪状态检查（2026-10-31）：`npm run typecheck`、`npm test`（51 个测试）和 `npm run build` 均通过。图页面会用普通状态行显示未加入保存图的核心，提示“保存的节点图中未启用”。完整输出保存在忽略 Git 的 `logs/test/20261031-stale-setup-recovery/`。

首页统一日志检查（2026-10-31）：`npm test`（54 个测试）、`npm run build`（类型检查及本地、托管构建）和 `git diff --check` 均通过。输出保存在忽略 Git 的 `logs/test/20261031-unified-logs-home/`。

旧编辑器移除检查（2026-10-31）：`npm test` 通过 50 项测试，`npm run build` 通过类型检查及本地、托管构建和清单检查，`git diff --check` 通过。完整输出位于忽略 Git 的 `logs/test/20261031-remove-legacy-editor/`。当前编辑器及 API 客户端仍有测试覆盖；删除的配置档案图测试仅对应已退役的编辑器。

仅添加节点集的预览检查（2026-10-31）：`npm test` 通过 51 项测试，`npm run build` 通过类型检查及本地、托管构建和清单检查，`git diff --check` 通过。完整输出位于忽略 Git 的 `logs/test/20261031-node-set-additive/`。测试覆盖预览删除现有节点时的拒绝行为。

IsOS v1 接口检查（2026-10-31）：`npm test` 通过 53 项测试，`npm run build` 通过类型检查及两种构建，`git diff --check` 通过。本地与托管清单都报告 IsosUI `0.1.0` 和 IsOS API 模式版本 `1`。完整输出位于忽略 Git 的 `logs/test/20261031-version-alignment/`。部署此资源包前，IsOS 服务端必须使用配套的 v1 接口。
