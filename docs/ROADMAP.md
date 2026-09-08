# 实施路线图

## 已实现：Phase 0 — Architecture Foundation

- CMake + C++20 分层工程；
- `IDataSource`、时间戳、`DataChunk`、`DataSample`；
- 通用环形缓冲与线程安全 `TimeSeriesStore`；
- 分级日志、`MockDataSource`、核心测试；
- Qt 空间与纯核心之间的依赖边界。

## 已实现：Phase 1 — Serial Debugger

- 串口枚举和全部常用参数；
- 独立 I/O 线程中的连接、断开、手动重连、RX/TX；
- ASCII/HEX 终端、毫秒时间戳、暂停显示、清空、复制、保存；
- ASCII/HEX 发送、历史、收藏、10 ms 起的循环发送；
- 独立线程 `.ldraw` 原始记录。

## 最小实现：Phase 2 — Text Plot

- 换行分隔 CSV 数值解析；
- 用户字段命名；
- 多字段曲线、字段开关、时间窗、自动/手动 Y 范围；
- Current/Min/Max/Average；
- 30 FPS 绘图与独立采集频率。

尚需补齐：历史拖动、区间框选、CSV 导出、可靠的高密度降采样、完善的坐标交互与长时间性能基准。

## 已实现：Phase 3 — Protocol Engine

- 不可变 `ProtocolDefinition` 与严格 JSON 加载；
- 流式 frame synchronizer：半帧、粘包、垃圾字节、错误长度、CRC 错误和重同步；
- 整数、浮点、布尔、字节数组、端序、scale、offset、unit、enum；
- sum8、CRC8/ATM、CRC16/MODBUS、CRC16/CCITT-FALSE；
- 固定长度与 `uint8/16/32` 动态长度字段；
- 首字节时间戳保留和解析统计。
- GUI 协议文件加载与停用、Packet Inspector、原始帧和字段查看；
- 有效帧、丢弃字节、校验/长度/解码错误统计；
- 数值字段自动进入统一时序存储与实时曲线；
- 协议热切换清空旧解析队列，原始记录链路保持独立。

## 已实现：Phase 4 — Recorder / Replay

- 目录式 Session 与版本化 `metadata.json`；
- 原始 RX/TX、数值 CSV、结构化帧 JSONL、事件和配置/协议快照；
- 停止记录处理屏障，保证已接收原始数据与解析值的尾部一致性；
- 小端可移植原始格式、长度防护、截断尾部恢复和中段损坏拒绝；
- 正式 `ReplaySource : IDataSource`，支持 0.1×~10×、暂停、继续、跳转；
- Replay 接入终端、协议引擎、TimeSeries 和曲线，自动恢复初始协议或 CSV 字段。

尚需强化：后台/缓存索引、超长 Session 压力测试、Session 浏览器、marker，以及回放结构化结果而不重新解析的可选模式。

## 已实现：Phase 5 — Network

- 与串口同样实现 `IDataSource` 的 `NetworkSource`；
- 独立 `QThread` 中的 TCP 客户端、单连接 TCP 服务端和 UDP socket 生命周期；
- TCP 双向字节流、UDP 双向数据报，RX/TX 均携带时间戳、方向、来源和序号；
- 网络配置页、连接状态、手动断开/重开和统一发送入口；
- 网络数据接入 Terminal、CSV/二进制协议、TimeSeries、Plot 与 Session；
- Session 保存网络模式、绑定端点和远端端点；
- 本机回环测试覆盖 TCP client/server 和 UDP 的精确收发内容。
- 独立 offscreen GUI 烟雾测试覆盖主窗口构造、事件循环和完整析构；CTest 固定使用项目 Qt，避免系统其他 Qt 版本干扰。

尚需强化：TCP 多客户端会话、主机名形式的 UDP 目标、自动退避重连、TLS、组播，以及网络高吞吐/长时间压力测试。

## 主链路已实现：Phase 6 — ROS2 / Remote Agent

已完成协议层与 Windows 客户端：

- 无 Qt/ROS 依赖的 Remote Agent v1 帧与 payload 数据模型；
- Hello/HelloAck、TopicCatalog、Subscribe/Unsubscribe、SampleBatch、Error 和心跳；
- 原始 CDR 与数值/布尔/文本字段同时保留；
- 源时间、Agent 接收时间、序号、CRC32 和固定长度上限；
- TCP 分片/粘包、垃圾、错误 CRC/版本/类型/长度后的流式重同步测试。
- 独立 `QThread` 的 Windows `RemoteAgentSource`、5 秒握手超时和严格入站序号检查；
- Windows 客户端可选指数退避自动重连、目录刷新与同端点订阅恢复；
- Windows 客户端主动 Ping/Pong 时钟测量、最低 RTT 偏移估计、质量 UI 与 Session 事件；
- Topic 目录、Agent 身份、订阅/取消订阅 UI 与手动刷新；
- CDR 原始记录、结构化数值/布尔曲线与 Session 元数据接线；
- 本机 TCP 模拟 Agent 回环测试覆盖握手、目录、样本、心跳和错误路径。
- 可移植的 Agent 服务端会话状态机及独立自动测试；
- Ubuntu/ROS2 Humble ament 包、POSIX TCP 单客户端服务端与 graph revision 推送；
- `GenericSubscription` 任意类型 CDR 转发；
- std_msgs、Twist/Pose、Imu/JointState、Odometry 常见字段映射。
- 未内置消息的运行时 C++/introspection typesupport 加载、递归字段展开、标准 Header 时间戳与原始 CDR 安全回退；
- 协商式 TopicFieldCatalog 能力扩展，在订阅前显示内置语义、通用解析、仅原始 CDR、不可订阅和失败原因，旧 v1 对端保持兼容；
- Ubuntu 22.04.5 + ROS2 Humble + GCC 11.4 构建、launch 与 SIGINT 验证；
- 真实 ROS graph、reliable/best-effort QoS、GraphUpdates、CDR、字段映射、订阅清理和重连自动联调；
- Linux loopback TCP 并发严格序号、CRC 恢复与协议错误自动测试。

下一批：

- 认证与 TLS 部署方式。

## 基础实现：Phase 7 — rosbag2

- Windows 无需安装 ROS2，使用 Qt QSQLITE 只读导入 rosbag2 `.db3`；
- 支持单文件和包含多个 SQLite 分卷的 bag 目录；
- 每卷前向读取并按时间戳稳定归并，导入内存不随消息总数增长；
- Topic、消息类型、序列化格式、消息数和来源数据库写入 Session 配置；
- 终端逐条显示 `sourceId`，导入回放可直接区分 Topic 与消息类型；
- 原始 CDR、时间戳和确定性序号进入标准 `.ldraw`，复用 0.1×~10×、暂停、跳转；
- 专用 rosbag2 路由保证 CDR 不进入 CSV 或自定义协议解析；
- 后台导入、进度、取消、临时目录清理、禁止覆盖和损坏/非 CDR 拒绝；
- 后台合并 Topic 目录，选择窗口支持搜索、消息计数、全选当前结果和清空；
- 正式导入只查询选中的 Topic，失效选择明确报错，非 CDR Topic 可见但不可选择；
- 无 ROS 依赖的 CDR1 大小端读取器，常见 std_msgs、Twist/Pose、Imu、JointState、Odometry 与 Remote Agent 使用相同字段路径和单位；
- 结构化值写入 `values.csv`，回放按原始记录实时进入曲线；未知/损坏消息保持原始 CDR 并记录映射失败；
- 自动测试覆盖分卷交错时间线、目录合并、字节保持、筛选、CDR 对齐/数组/损坏回退、结构化回放、取消和错误路径。

已补充：`metadata.yaml` 原文完整保存、稳定字段摘要、SHA-256 及 SQLite 文件/消息数交叉校验。

下一批：更多消息/可选动态类型描述、MCAP/压缩存储，以及超长 bag 的后台/稀疏回放索引。

后续：

- Phase 8 已完成基础块：正式 `SourceManager`、串口/网络/Remote Agent 同时连接和记录、聚合统计、按 `sourceId` 隔离 CSV/二进制解析、同名曲线来源限定，以及明确的串口/网络发送目标。
- Phase 8 已完成派生变量块：安全表达式 AST、白名单函数、循环依赖检查、复杂字段反引号引用、实时曲线/统计、Session 配置冻结及确定性回放。
- Phase 8 已完成基础滤波与时域变换：低通、高通、移动平均、微分、梯形积分、角度展开、事务式状态更新，以及记录/回放/跳转重置语义。
- Phase 8 下一批：同类型数据源动态增删、逐来源协议配置、marker、告警和更完整的多源同步分析。

每阶段必须先补数据格式、线程与错误路径测试，再扩 UI。性能验证重点是 921600 baud、1000 Hz 数值流、数小时记录和 UI 暂停期间的数据完整性。
