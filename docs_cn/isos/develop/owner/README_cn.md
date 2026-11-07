# 归属方实现说明

本目录面向 IsOS 本地管理器及所配置服务的实现者。这些是内部控制面；软件包和核心发布者应使用[外部核心契约](../../interfaces/cores/README_cn.md)进行交接和停止。

- [管理器](manager_cn.md)定义本地操作 IPC 与检查修订的变更封套。
- [服务](services_cn.md)定义选择本地归属方与启动子进程的管理器 TOML 条目。
- [日志游标](log-cursors_cn.md)定义用于诊断的运行范围日志分页及排序。

[English](../../../../docs/isos/develop/owner/README.md)
