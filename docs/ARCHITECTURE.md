# Lab Debugger 架构设计

## 1. 设计判断

项目采用 **Core / Adapter / Application / UI** 四层，而不是把业务逻辑放进 `MainWindow`。

```text
Serial / TCP / UDP / Replay / ROS2 / Remote ROS Agent
                         │
                         ▼
                    IDataSource
                         │ DataChunk
             ┌───────────┼────────────┐
             ▼           ▼            ▼
        Raw recorder  Processing   UI event queue
                         │            │ 30 Hz batch
                    ┌────┴────┐       ▼
                    ▼         ▼    Terminal
               DataSample  FrameEvent
                         │
                         ▼
                  TimeSeriesStore
                         │ snapshot
                         ▼
                       Plot
```

`IDataSource` 表达打开、关闭、状态、写入、原始数据、可选结构化样本、错误与统计。上层从 `DataChunk` 中得到来源、方向、源时间、接收时间、序号和字节；Remote Agent 还可直接发布已经由 Agent 展平的 `DataSample`，避免把 CDR 误送入 CSV 解析器。

核心库不用 Qt 类型，便于独立测试、用于无界面 Agent，或未来抽成基础设施库。Qt 目前只出现在串口/网络适配器和桌面界面。

## 2. 时间与数据语义

- `sourceTimestamp`：数据源能提供时采用设备/ROS 消息时间；普通串口使用接收时刻。
- `receiveTimestamp`：Lab Debugger 收到数据的本机 Unix 纳秒时间。
- `sequence`：每个数据源单调递增。
- `sourceId`：全局稳定来源标识，为未来多源时间轴保留。
- `DataSample.timestamp` 默认继承源时间，解析字段可直接进入统一时序存储。

所有记录同时保留原始数据和解析结果的扩展位置。当前 `.ldraw` 二进制格式为：

```text
8 bytes  magic "LDBGRAW1"
repeat:
  uint32 record magic 0x4C444252
  int64  source timestamp ns
  int64  receive timestamp ns
  uint64 sequence
  uint8  direction (0 RX, 1 TX)
  uint32 source id bytes
  uint32 payload bytes
  source id UTF-8
  payload
```

多字节整数明确以小端写入。读取器验证文件头、记录标记、方向和长度上限；尾部截断可恢复，中段损坏会拒绝加载。Session 元数据独立携带格式版本。

## 3. 线程模型

```text
GUI thread                 只处理交互、33 ms 批量终端刷新和绘图快照
Serial QThread             QSerialPort 的 open/read/write/error 生命周期
Network QThread            QTcpSocket/QTcpServer/QUdpSocket 生命周期
Remote Agent QThread       QTcpSocket、握手、帧解码、订阅控制、心跳和时钟测量
Processing std::jthread    按 sourceId 隔离的 CSV/二进制帧解析、TimeSeries 追加、FrameEvent
Recorder std::jthread      有序写入 RX/TX 原始记录
Session std::jthread       异步写入数值、结构化帧、事件和配置快照
rosbag2 std::jthread       Qt Sql 前向读取、分卷时间归并与同步 RawLogWriter
```

暂停终端或曲线只影响绘制，不会停止串口、处理线程或记录线程。`TimeSeriesStore` 使用共享锁保护；绘图得到有序快照，不持有内部存储引用。

当前处理和记录队列选择可靠性优先，不在压力下静默丢弃。`SourceManager` 用稳定 key 管理每个 `IDataSource`，聚合统计并把数据扇出到同一记录器；`ProcessingPipeline` 则为每个 `sourceId` 保留独立半行/半帧状态，任意交错到达都不会跨设备拼接。CSV 每一行、协议每一帧以及 Remote Agent 每个 SampleBatch 还保留明确的结构化批次边界，使多字段派生计算和滤波状态只推进一次。数值序列使用“`sourceId.字段`”命名，因此不同设备的同名字段不会混成一条曲线。停止 Session 时，数据源路由短暂进入屏障，等待处理队列变空后再结束记录，确保停止点前的原始数据和解析值一致。队列深度会暴露为指标；后续持续压力策略是背压告警、分块落盘和可配置内存上限，而不是无提示截断。

`ReplaySource` 实现同一个 `IDataSource` 接口，按原始接收时间调度 `DataChunk`，所以回放复用 Monitor、Protocol 和 Plot 全链路。跳转会暂停回放、等待处理队列空闲、重置流解析器后再切换索引位置。

`Rosbag2Importer` 位于 Qt 适配器层：QSQLITE 只负责读取 rosbag2 数据库，纯核心 `RawLogWriter` 负责流式生成标准 `.ldraw`。预检先合并分卷 Topic/类型/格式与计数，UI 再提交明确的 Topic 选择；正式查询按各数据库本地 topic id 限定数据。分卷 bag 同时保留每卷一个前向游标，并按时间戳、文件序和行号做确定性归并，因此导入内存取决于分卷数和单条消息大小，而不是消息总量。`metadata.yaml` 作为不可信的辅助输入：限制大小后原样保存，稳定字段用于同 SQLite 文件集合和消息总数交叉校验，但从不决定打开哪些数据库或覆盖查询计数。没有可信结构化采样时 Session 标记为 `raw-only`；存在内置字段时标记为 `rosbag2-structured`。两种模式都让 CDR 绕过普通 CSV/自定义协议处理，后者仅通过专用 CDR 映射器进入曲线。

## 4. Windows / Linux / ROS2 解耦

主 GUI 的必选依赖只有 Qt Widgets 和相应数据源模块。ROS2 不属于 `lab_core`，也不属于 Windows 基础构建。

```text
Linux local mode:
  Ros2Source adapter -> rclcpp / introspection -> IDataSource

Windows remote mode:
  ROS2 + Lab Debug Agent (Linux)
       -> framed TCP protocol
       -> RemoteAgentSource (Windows, no ROS2 dependency)
       -> IDataSource
```

远程帧必须携带 topic、message type、源时间、Agent 接收时间、序号与负载。当前 Agent 使用 GenericSubscription 转发任意已安装 typesupport 的原始 CDR；常用消息先走带单位和专用语义的编译期映射，其余消息由运行时 C++ typesupport 反序列化，再根据 introspection metadata 递归展开字段。ROS2 构建通过独立 ament 包启用，不向核心传播头文件或链接依赖。

Remote Agent v1 帧和负载编解码位于纯 C++ `lab_core`。Windows `RemoteAgentSource` 在独立 Qt 网络线程内执行客户端状态机、指数退避重连、同端点订阅恢复和 Ping/Pong 时钟测量；纯核心 `ClockSyncEstimator` 在最近 16 个样本中选择最低 RTT 样本，给出 Agent 相对客户端的时钟偏移和不确定度。同一核心中的 `ServerSession` 执行 Agent 侧能力协商、客户端命令、统一序号和错误状态机。Ubuntu ament 包把 POSIX TCP 传输、ROS graph、`GenericSubscription`、常见消息语义映射与通用 introspection 组合在服务端状态机外部。协商 `TopicFieldCapabilities` 后，独立的 `TopicFieldCatalog` 与基础目录使用相同 graph revision，把类型探测结果送到 UI；未协商时仍只发送旧格式 `TopicCatalog`。SampleBatch 的原始 CDR 进入 Raw Recorder，数值与布尔字段直接进入 TimeSeries 和 Session；时钟质量进入 Session 事件。详细格式见 [Remote Agent 协议](REMOTE_AGENT_PROTOCOL.md)。Linux 包此前已经在 Ubuntu 22.04.5 + ROS2 Humble + GCC 11.4 下完成自动构建和真实 ROS/DDS/TCP 联调。

rosbag2 离线链路不把 ROS2 运行时带入 Windows 客户端。Qt QSQLITE 负责 Topic 预检和前向消息游标，纯 C++20 `cdr_field_mapper` 按标准 CDR1 对齐和大小端规则读取常见消息，并复用 Agent 的字段路径与单位。导入阶段把数值写入 `values.csv`，同时始终保存原始 `.ldraw`；回放阶段以原始记录的时间轴、暂停和跳转为准，专用映射路由直接把可信数值送到 `TimeSeriesStore`。未知类型、资源超限和损坏 CDR 不进入普通 CSV/协议处理链，只保留原始字节。

## 5. 协议引擎边界

`FrameStreamParser` 消费 `DataChunk`，处理固定帧头、固定/动态长度、校验与流重新同步；`ProtocolDecoder` 再按经过严格校验的 `ProtocolDefinition` 解码字段。两层分开，避免每个 STM32 协议重复实现状态机，也使 CRC 错误不会污染字段层。

JSON 是当前内建零依赖格式，加载失败时返回结构化问题且不替换运行中的协议。处理线程把数值字段写入 `TimeSeriesStore`，同时把批量 `FrameEvent` 交给 GUI Packet Inspector。热切换会丢弃模式切换前尚未解析的旧队列，原始 `.ldraw` 记录不受影响。未来可增加 YAML 前端并统一转换为同一个 `ProtocolDefinition`。

`DerivedFieldEngine` 位于纯核心层，使用受限 AST 解析四则运算、括号、常量和固定白名单函数，不执行任意脚本。配置阶段限制表达式长度、节点数和嵌套深度，并拒绝重复名、保留名及派生字段循环；配置失败不会替换当前有效定义。运行阶段只保存有限数值，按拓扑顺序计算受影响字段，任何缺失依赖、数学定义域错误或非有限结果都会让对应输出和下游失效。来源限定字段通过反引号精确引用，避免多源同名字段被猜测合并。

低通、高通、移动平均、微分、积分和角度展开的状态绑定到各自 AST 函数节点。一次求值先生成待提交状态，只有整条表达式成功或微分正常预热时才原子提交；外层除零或非法参数不会使内部滤波器偷偷前进。配置替换、Session 开始、回放打开和跳转都会清空节点状态，保证原始输入相同时实时与回放结果一致。

`ThresholdAlertEngine` 同样位于纯核心层。规则只按完整字段名匹配有限数值，以高于/低于阈值的边沿进入告警，并用非负回差控制复位；锁保护的规则状态可同时接收串口/网络处理线程和 Remote Agent 样本。配置限制为 128 条并采用事务式替换，无效配置不会破坏当前有效规则。应用层把人工 Marker 和触发结果统一转换为 `SessionEvent`，实时绘图只消费只读快照；历史回放直接恢复事件，不重新执行告警，从而避免倍速与 seek 产生重复告警。

## 6. 生命周期与多数据源

应用层已使用 `SourceManager` 同时管理 `SerialSource`、`NetworkSource` 与 `RemoteAgentSource`：三者可以独立连接/断开，终端按 `sourceId` 区分，流解析器互相隔离，状态栏显示聚合流量，Session 在开始时冻结并记录全部已连接源。记录期间允许已声明源按原配置断开和重连，但拒绝加入新配置，避免原始记录出现元数据未声明的来源。`ReplaySource` 与实时源互斥，打开回放会先关闭全部实时源并复用相同的显示、解析和曲线边界。

派生配置、告警规则与源集合分别冻结到 `configuration/derived_fields.json`、`configuration/alert_rules.json` 和逐源配置。开始记录、打开回放及跳转都会重置派生状态与告警锁存状态；迟到且未声明的数据源可以继续显示，但不能改变已冻结 Session 的派生结果。普通回放从原始数据重新计算，结构化 rosbag2 则以同一条消息的映射字段作为一个批次计算；Marker 和已发生告警直接从事件日志恢复。当前跨来源表达式采用最近有效值语义，严格的公共时间轴插值仍属于后续同步层。

## 7. 当前已知边界

- 当前绘图是自绘 Qt Widget，不依赖 Qt Charts；已使用桶内 min/max 降采样保留尖峰，适合 10~20 条常规曲线，但尚未实现 LTTB、GPU 加速和超高密度交互。
- 当前 UI 各提供一个串口、一个网络端点和一个 Remote Agent 实例；核心管理器可扩展更多实例，但多串口/多网络端点的动态增删界面尚未实现。0.19 起 `ProcessingPipeline` 按完整 `sourceId` 解析：每个来源可覆盖默认 CSV 字段或二进制协议，并拥有独立半行/半帧状态；Session 会冻结每个来源解析后的最终配置。
- 派生变量已提供基础算术、数学函数及低通、高通、移动平均、微分、积分和角度展开；运行期错误计数、更高级滤波器和严格多源时间对齐仍待后续滤波/同步阶段实现。
- 告警当前提供高于/低于阈值、回差复位、曲线 Marker 与 Session 回放；持续时间、组合逻辑、CRC 错误率、Topic 超时、声音和确认/消音流程仍待扩展。
- 回放索引当前在打开文件时同步建立，每条原始记录占 16 字节索引内存；超长 Session 的后台建索引与稀疏缓存仍待实现。
- rosbag2 当前支持 SQLite3/CDR、Topic 过滤、一组常见消息的离线结构化，以及 `metadata.yaml` 全文保存和稳定字段校验；MCAP、压缩存储、通用 YAML 编辑、动态类型描述及更多消息仍待实现。
- CSV 解析器只处理换行分隔的数值；二进制帧、单位和校验由独立协议引擎处理。
- 串口断开后提供手动重连；自动退避重连和端口热插拔恢复留到后续。
- TCP 当前服务一个活动客户端；UDP 远端目前要求数字 IPv4/IPv6 地址。自动重连、TLS、组播和多客户端管理留到后续。
- 多机时钟偏移采用单次 Ping/Pong 的对称链路近似；界面显示的 ±RTT/2 是误差上界提示，不能替代 NTP/PTP，也不会修改任一主机的系统时钟。
- Remote Agent v1 暂无认证或加密，只允许受信网络/VPN/SSH 隧道；TLS 与访问控制留在 Agent 联调阶段。
- 通用 ROS 字段展开限制为 32 层、每个数组 1024 项、每条样本 4096 个字段；达到边界时保留已验证字段、记录告警并继续发送原始 CDR。
