# rosbag2 导入与回放

Lab Debugger 0.11 提供 Phase 7 的第一版 rosbag2 离线链路：Windows 客户端无需安装 ROS2，即可把 rosbag2 SQLite 存储转换成标准 Lab Debugger Session，再复用现有回放时间轴。

## 使用方法

1. 打开“Session 回放”页。
2. 单个 SQLite 文件点击“导入 .db3”；标准 bag 或分卷 bag 点击“导入 bag 目录”。
3. 选择导入后 Session 的保存位置。
4. 导入在后台执行，可查看消息进度或点击“取消导入”。
5. 成功后程序自动打开生成的 Session，默认保持暂停；可按 0.1×~10× 播放或拖动时间轴。

导入目标使用新目录，绝不会覆盖已经存在的 Session。导入未完成、输入损坏或用户取消时，临时目录会被清理，不会发布半成品 Session。

## 数据与时间语义

- 支持 rosbag2 默认 SQLite3 的 `topics` / `messages` schema 和 `cdr` 序列化格式。
- 选择 bag 目录时会发现其中所有 `.db3` 分卷，以每卷一条前向查询做时间归并；内存不会随消息总数线性增长。
- rosbag 的消息时间戳同时写入 Session 的 `sourceTimestamp` 与 `receiveTimestamp`，同一时间戳按数据库文件顺序和行号稳定排序。
- 每条记录的 `sourceId` 同时携带 topic 名和 ROS 消息类型，终端行会显示这个来源；原始 CDR 字节逐字节保留。
- `metadata.json` 标记 `replay_mode: raw-only`，`configuration/rosbag2.json` 保存来源数据库、Topic、类型、序列化格式和消息计数。

CDR 是 ROS 的二进制序列化数据，不是 CSV 文本。导入的 Session 因此只把 CDR 送到原始终端与回放链路，不会进入 CSV/二进制自定义协议解析器，也不会产生伪曲线。后续版本会在不破坏原始记录的前提下增加可选 Topic 过滤、常见消息离线结构化和索引缓存。

## 安全限制与错误处理

- 单条原始负载上限与 Session 一致，为 64 MiB；Topic 名或类型也有长度上限。
- 非 `cdr` 序列化、缺少标准表/列、非法时间戳、数据库读写错误会明确拒绝导入。
- 当前不支持 MCAP、压缩 bag、加密存储和 ROS1 `.bag`。
- 本版不解析 `metadata.yaml` 中的所有 QoS/自定义元数据；SQLite Topic/类型目录仍会完整保留在导入结果中。
- 当前回放仍会在打开 Session 时同步建立逐记录内存索引；超长 bag 的后台/稀疏索引属于下一批优化。

## 开发依赖

GUI 构建比此前多使用 Qt Sql 和随 Qt 部署的 QSQLITE 驱动。无 GUI 构建仍不依赖 Qt、SQLite 或 ROS2。
