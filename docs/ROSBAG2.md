# rosbag2 导入与回放

Lab Debugger 0.11 提供 Phase 7 的第一版 rosbag2 离线链路，0.12 增加导入前 Topic 预检与筛选，0.13 增加常见 ROS 消息的无 ROS 依赖 CDR 解析与曲线回放：Windows 客户端无需安装 ROS2，即可把需要的 rosbag2 SQLite 数据转换成标准 Lab Debugger Session，再复用现有回放时间轴。

## 使用方法

1. 打开“Session 回放”页。
2. 单个 SQLite 文件点击“导入 .db3”；标准 bag 或分卷 bag 点击“导入 bag 目录”。
3. 选择导入后 Session 的保存位置，程序在后台读取并合并 Topic 目录。
4. 在 Topic 选择窗口查看“原始 CDR + 曲线”或“仅原始 CDR”能力，并可搜索、全选当前结果或清空选择；非 CDR Topic 会显示但不可选择。
5. 确认后只导入选中 Topic，可查看消息进度或点击“取消导入”。
6. 成功后程序自动打开生成的 Session，默认保持暂停；可按 0.1×~10× 播放或拖动时间轴。

导入目标使用新目录，绝不会覆盖已经存在的 Session。导入未完成、输入损坏或用户取消时，临时目录会被清理，不会发布半成品 Session。

## 数据与时间语义

- 支持 rosbag2 默认 SQLite3 的 `topics` / `messages` schema 和 `cdr` 序列化格式。
- 选择 bag 目录时会发现其中所有 `.db3` 分卷，以每卷一条前向查询做时间归并；内存不会随消息总数线性增长。
- 预检会合并同名同类型 Topic 的分卷计数；正式导入只查询并写入选中的 Topic，本次 Session 的计数与目录也只包含选中内容。
- rosbag 的消息时间戳同时写入 Session 的 `sourceTimestamp` 与 `receiveTimestamp`，同一时间戳按数据库文件顺序和行号稳定排序。
- 每条记录的 `sourceId` 同时携带 topic 名和 ROS 消息类型，终端行会显示这个来源；原始 CDR 字节逐字节保留。
- `metadata.json` 使用 `replay_mode: rosbag2-structured` 或 `raw-only`，`configuration/rosbag2.json` 保存来源数据库、Topic、类型、序列化格式、消息计数和字段映射能力。
- 结构化字段同时写入 `values.csv` 便于分析导出；回放时仍以 `.ldraw` 原始记录为调度主线，按 Topic 类型安全解析后直接进入 `TimeSeriesStore`，不会把二进制送入 CSV 或自定义协议解析器。

CDR 是 ROS 的二进制序列化数据，不是 CSV 文本。0.13 内置 `std_msgs/msg/Float32`、`Float64`、`Int32`、`UInt32`、`Bool`，`geometry_msgs/msg/Twist`、`TwistStamped`、`PoseStamped`，`sensor_msgs/msg/Imu`、`JointState` 和 `nav_msgs/msg/Odometry`。字段路径与 Remote Agent 保持一致，并保留 `m`、`m/s`、`rad`、`rad/s`、`m/s^2` 等单位；曲线字段带 Topic 前缀，避免多个 Topic 的 `data` 或 `pose` 相互覆盖。Header 时间戳非零时用于曲线，零时间戳回退到 bag 消息时间。

所有类型都会逐字节保留原始 CDR。未知类型、非有限数值、单条截断/损坏消息或不支持的 CDR 封装不会产生伪字段；一条已知消息中只要有一个 NaN 或 Inf，整条消息的结构化字段都会清空，不会保留其他有限字段形成部分曲线。其中未知类型保持“仅原始 CDR”，已知类型解析失败会计入 Session 的 `mapping_failure_count` 并在回放时按来源限频提示。后续版本会增加更多类型、可选动态类型描述和索引缓存。

## 安全限制与错误处理

- 单条原始负载上限与 Session 一致，为 64 MiB；Topic 名或类型也有长度上限。
- 变长字符串上限 1 MiB，JointState 等序列单项上限 1024；超限消息只保留原始 CDR。
- 当前离线映射支持标准 CDR1 大小端封装；其他封装表示会安全回退，不尝试猜测布局。
- 非 `cdr` 序列化、缺少标准表/列、非法时间戳、数据库读写错误会明确拒绝导入。
- 当前不支持 MCAP、压缩 bag、加密存储和 ROS1 `.bag`。
- 本版不解析 `metadata.yaml` 中的所有 QoS/自定义元数据；SQLite Topic/类型目录仍会完整保留在导入结果中。
- 当前回放仍会在打开 Session 时同步建立逐记录内存索引；超长 bag 的后台/稀疏索引属于下一批优化。

## 开发依赖

GUI 构建使用 Qt Sql 和随 Qt 部署的 QSQLITE 驱动。离线 CDR 字段映射本身是纯 C++20，不链接 ROS2；无 GUI 构建仍不依赖 Qt、SQLite 或 ROS2。

ROS2 Agent 测试包中的 `lab_debug_agent_cdr_compatibility_tests` 会使用 Humble 的 `rclcpp::Serialization<T>` 生成真实 CDR，再直接编译并调用客户端共享的 `cdr_field_mapper.cpp`，同时逐项比较 Remote Agent 字段路径、单位、数值和 Header 时间戳。该目标只属于 ROS 测试体系，不会把 ROS2 依赖传播到 `lab_rosbag_cdr`、`lab_core` 或 Windows GUI。
