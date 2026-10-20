# 运行时私有存储

[English](../../../../docs/eos/design/runtime/store.md)

`runtime/store/` 保存少量强类型的 EOS 私有状态，不提供通用 SQL 数据库或任意键值 API。首个 collection 按机器人保存绝对平面导航点，例如：

```json
{
  "format_version": 1,
  "robots": {
    "robot-0": {
      "navigation_points": {
        "example": {"x_m": 1.0, "y_m": 2.0, "yaw_rad": 0.0}
      }
    }
  }
}
```

文件位于 `${ISLAND_STATE}/eos/custom-store/navigation-points.v1.json`，或 checkout state 回退目录。EOS 以 `0700` 创建目录，通过 `0600` 临时文件、`fsync` 和 rename 原子写入快照。名称和机器人 ID 是长度受限的路径段，坐标必须有限。读取时拒绝未知字段、不支持的版本、符号链接、过大文件和超过 10,000 个点。

运维人员可以直接编辑 JSON。EOS 在任何 API 修改前检测文件变化，若已改变则返回冲突，避免覆盖外部修改。编辑后调用 `POST /v1/eos/navigation/waypoints:reload`，或在 REPL 中运行 `waypoint reload`。无效编辑会拒绝加载，并保持内存快照不变。

保存的点还可以通过 `robots/{robot}/navigation/waypoints/{name}` 读取。该投影为现有 `navigate_waypoint@1.0.0` 技能提供 `reference: absolute` 和 `frame_id: map`；磁盘只保存 `x_m`、`y_m`、`yaw_rad`。强类型 API 支持列出、读取、更新或插入、删除。在单个 EOS 进程内采用最后写入者生效，不支持并发外部写入者。
