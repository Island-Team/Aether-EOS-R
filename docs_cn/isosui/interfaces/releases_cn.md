# IsosUI 0.1.0 发布与 Island-web 推进

[English](../../../docs/isosui/interfaces/releases.md)

**所有权：** IsosUI 负责源码标签、本地和托管构建归档、SHA-256 校验值及发布版本。Island-web 负责固定版本、网站构建、PR 和部署。IsosUI 发布成功不等于公开 HTTPS 页面已经部署。

仅在评审后的源码合并到 IsosUI `master`、CI 通过，且准确的 `v<package.json version>` 标签指向当前 master 提交后，手动运行 **Release IsosUI**。跨仓库 dispatch/mirror 令牌不可用时选 `promotion=manual`；只有 IsosUI 已配置 `ISLAND_WEB_DISPATCH_TOKEN` 且 Island-web 已配置 `ISOSUI_MIRROR_TOKEN` 时才选 `promotion=dispatch`。工作流在发布前拒绝错误版本、偏离当前 master 的标签、已存在的发布版本及缺失的 dispatch 令牌。随后使用 Node 22 运行测试、发布负向测试、类型检查、两个构建及构建清单验证。在 `gh release create` 之前再次核对远端 `master` 提交、远端标签解引用后的提交及发布版本是否仍不存在，防止构建期间状态发生变化。检查两个归档的摘要和内含 `manifest.json` 身份后，准确发布本地和托管归档及 `SHA256SUMS`。

手动推进 Island-web：

1. 下载 IsosUI 发布页的三个资源。运行 `sha256sum -c SHA256SUMS`，记录托管归档摘要、发布标签及准确的标签提交。
2. 从当前 Island-web `master` 建立 `refactor(v2)/<topic>` 分支，把 `scripts/isosui-release.json` 更新为 `{ "source_commit": "<标签的 40 位提交 SHA>", "release_tag": "<标签>", "api_schema_version": 1, "sha256": "<托管归档 SHA-256>" }`。固定的 API schema 必须与托管归档清单一致。安装脚本会检查归档摘要且只解包安全路径。不要用按提交构建的资源代替发布归档。
3. 在 Island-web 运行 `npm ci --no-audit --no-fund`、`npm run build`，并检查 `dist/en/isos/index.html`、`dist/cn/isos/index.html`、`dist/isosui/manifest.json`，以及托管清单的版本、源码提交和 `build_target: hosted`。将输出保存在 `logs/test/<run-id>/`，向 Island-web `master` 提交 PR，评审后合并。
4. 网站部署成功后，检查公开的 `/en/isos`、`/cn/isos` 页面及 `/isosui/` 资源是否与固定的摘要和清单一致。连接可用的本机 IsOS/EOS 后，重放首次使用流程：Validate/Apply、就绪、关联的原语结果、当前运行日志和 Stop。HTTPS 页面访问本机环回服务可能需要浏览器的本地网络权限。

dispatch 路径只要求 Island-web 的镜像工作流创建固定版本 PR，并不直接部署页面。如果发布后 dispatch 失败，发布版本仍然存在，可用相同的已验证资源手动推进。不要对已存在的标签再次创建发布版本。

## Q04 发布前证据

该专题在 Linux 的 Node 22.23.4 和 npm 10 环境下检查。从准确的已提交 HEAD 重放 `npm ci --no-audit --no-fund`、`npm test`、`node --test tests/release-preflight.node.mjs tests/release-boundary.node.mjs`、`npm run typecheck`、`npm run build` 和 `git diff --check`。发布测试建立本地和托管归档样本，拒绝被改动的字节、错误的内嵌源码提交、符号链接成员、缺失或重复的校验项、错误版本或模式及缺失的 dispatch 令牌。发布边界测试使用本地 Git 远端验证轻量标签和附注标签；远端 master 前进、远端标签改指或发布版本已存在时拒绝创建。构建验证要求两个清单都写入准确的 HEAD。完整本地输出位于被忽略的 `logs/test/20261104-q04-release-preflight/` 和 `logs/test/20261104-q04-release-boundary/`。该发布前专题本身没有创建标签、发布版本或 Island-web 部署；后续实际发布记录如下。

## 已发布 v0.1.0 的证据

附注标签 [`v0.1.0`](../../island/retired-repository-references_cn.md#ref-eb78d2736c67) 解引用至 IsosUI `master` 提交 `ff4f8800f44485325d767e56fbf1ea10c2046d68`。[发布工作流 41709462695](../../island/retired-repository-references_cn.md#ref-68283862edb9) 从该提交运行成功，发布两个归档及 `SHA256SUMS`。从发布页下载的资源校验值如下；先前本地构建的发布前归档字节不同，不能替代这些资源：

| 发布资源 | SHA-256 |
| --- | --- |
| `isosui-local-v0.1.0.tar.gz` | `961d0152e02b0aa45882b9f5a1bcc83990bb062324f4453ff82cc3c6c8f3feca` |
| `isosui-hosted-v0.1.0.tar.gz` | `c95152eccd5d668dacc4be2e10675c0ad3537d2221ba79acc4dc4db41a449799` |

两个归档中的 `manifest.json` 均声明 `schema_version: 1`、`version: "0.1.0"`、上述精确源码提交、`api_schema_version: 1` 以及各自的 `build_target`（`local` 或 `hosted`）。下载的 `SHA256SUMS` 已通过 `sha256sum -c SHA256SUMS`。本地校验记录与下载资源位于 `/home/operator/q04t/isosui-published-record/`。

Island-web 经评审的[固定版本 PR #138](https://github.com/Island-Team/Island-web/pull/138) 合并为 `a5c20eb527634c9e8be3aed80a40278e15ffbb02`，固定了该托管摘要、标签、提交及 API schema。[部署工作流 41711195077](https://github.com/Island-Team/Island-web/actions/runs/41711195077) 运行成功。工作流结果证明网站部署，不单独证明真实浏览器完成了回环所有者首次使用流程。Q03 与合并后 Q04 浏览器回放使用回环地址模拟托管 Origin；真实公网 HTTPS 浏览器与跨所有者发布回放仍是独立验证证据。
