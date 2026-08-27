# Remote ROS Agent 通信协议

Remote Agent 协议用于以下部署方式：

```text
Ubuntu / ROS2 Humble                  Windows / Linux
┌────────────────────┐               ┌────────────────────┐
│ Lab Debug Agent    │   TCP         │ Lab Debugger       │
│ rclcpp + ROS graph ├──────────────►│ RemoteAgentSource  │
└────────────────────┘               └────────────────────┘
```

协议核心位于 `lab_core`，不依赖 Qt、ROS2 或操作系统 API。这样 Windows 主程序不需要安装 ROS2，Linux Agent 也能复用同一套编解码器和测试向量。

当前已完成协议核心、Windows `RemoteAgentSource`、Topic UI、Session/曲线接线，以及 Ubuntu/ROS2 Humble Agent。共享状态机、Linux TCP 服务端和真实 ROS/DDS/TCP 链路均已有自动测试；ament 包已在 Ubuntu 22.04.5 + ROS2 Humble + GCC 11.4 下验证。

## 1. TCP 帧

所有多字节整数和 IEEE 754 `double` 均使用小端编码。固定帧头为 40 字节：

| 偏移 | 大小 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 4 | magic | ASCII `LDRA` |
| 4 | 1 | version | 当前为 `1` |
| 5 | 1 | message type | 见消息类型表 |
| 6 | 2 | flags | 当前保留，发送端写 0 |
| 8 | 4 | payload bytes | 最大 16 MiB |
| 12 | 8 | sequence | 每条 Agent 连接单调递增 |
| 20 | 8 | source timestamp | ROS 消息/设备源时间，Unix ns；没有源时间时用 Agent 回调时间 |
| 28 | 8 | Agent receive timestamp | Agent 收到 ROS 回调或命令的 Unix ns |
| 36 | 4 | CRC32 | CRC32/IEEE，覆盖帧头 0..35 与 payload，不覆盖 CRC 字段本身 |
| 40 | N | payload | 按消息类型编码 |

客户端收到帧时还会生成自己的 `receiveTimestamp`。因此一条远程样本最终具有三层时间：源时间、Agent 接收时间、Lab Debugger 接收时间，可用于估计传输延迟和多机时钟质量。

解码器支持 TCP 任意分片和粘包。发现垃圾、错误版本、未知类型、超长声明或 CRC 错误时会记录结构化问题并搜索下一个 `LDRA`，不会把损坏数据交给上层。长度在分配前验证，单帧 payload 硬上限为 16 MiB；字符串上限 1 MiB，集合最多 100000 项。

## 2. 消息类型

| 值 | 名称 | 方向 | 用途 |
|---:|---|---|---|
| 1 | Hello | Agent → Client | Agent 身份、版本、主机和能力位 |
| 2 | HelloAck | Client → Agent | 客户端身份、版本和请求能力 |
| 3 | TopicCatalog | Agent → Client | ROS graph revision 与 topic/type/QoS 列表 |
| 4 | Subscribe | Client → Agent | 订阅 topic、类型、可靠性和队列深度 |
| 5 | Unsubscribe | Client → Agent | 使用同一请求结构取消订阅 |
| 6 | SampleBatch | Agent → Client | 原始序列化消息与扁平化字段 |
| 7 | Error | 双向 | 错误码、上下文和消息 |
| 8 | Ping | 双向 | 64 位 nonce 心跳 |
| 9 | Pong | 双向 | 原样返回 nonce |
| 10 | TopicCatalogRequest | Client → Agent | 主动请求最新 TopicCatalog，payload 为空 |

字符串和字节数组采用 `uint32 length + bytes`；集合采用 `uint32 count`。字符串均为 UTF-8。

## 3. Topic 与订阅

`TopicCatalog` 包含单调变化的 `graphRevision`。每个条目保存：

- topic 完整名称；
- ROS 接口类型，例如 `sensor_msgs/msg/Imu`；
- reliability：unknown / best effort / reliable；
- durability：unknown / volatile / transient local。

Linux Agent 使用 ROS2 Humble 的 `get_topic_names_and_types()` 建立目录，并按 topic/type 分别汇总发布端 endpoint QoS，生成可靠性与持久性提示。首次目录（包括空目录）的 revision 从 1 开始；topic、type 或汇总 QoS 变化时递增。

订阅请求包含独立 `requestId`，用于标识控制请求，不作为活动订阅的唯一键。活动订阅按 `topic + type` 区分：相同 QoS 的重复订阅是幂等操作，更改 QoS/queueDepth 会替换该 topic/type 的订阅，取消订阅只移除完全匹配的 topic/type，不会误删同名的其他类型。响应失败时 `Error.context` 包含 topic。队列深度范围固定为 1..1000000。

## 4. SampleBatch

一次 SampleBatch 保存：

- topic；
- ROS message type；
- 原始序列化 CDR 字节；
- 零个或多个扁平化字段。

字段路径使用 ROS 成员层级，例如：

```text
pose.pose.position.x
twist.twist.angular.z
orientation.w
```

字段值当前支持：

- `double`：所有可安全转换的数值类型，进入 TimeSeries、Plot 和 values.csv；
- `bool`：保留布尔语义，可同时映射为 0/1 曲线；
- UTF-8 string：用于 frame id、枚举文本和检查器。

保留原始 CDR 是为了未来修复 introspection 或字段映射后能够重新分析，而不是只相信当时的扁平化结果。

Agent 只发送客户端已协商的内容：`SerializedMessages` 控制原始 CDR，`NumericFields` 控制数值和布尔字段，`TextFields` 控制字符串字段。没有协商 `TopicDiscovery` 时不发送目录；`GraphUpdates` 必须与 `TopicDiscovery` 一起协商。过滤后没有任何内容的样本不会发送。

## 5. 连接状态机

推荐顺序：

```text
TCP connected
  → Agent sends Hello
  → Client validates version/capabilities and sends HelloAck
  → Client sends TopicCatalogRequest
  → Agent sends TopicCatalog
  → Client sends Subscribe / Unsubscribe
  → Agent streams SampleBatch and graph updates
  ↔ Ping / Pong
```

尚未完成 Hello/HelloAck 时不应接受订阅命令。客户端不得请求 Agent 未提供的能力，也不得在未协商任何样本能力时订阅；若 Agent 错误地只声明 `GraphUpdates` 而没有 `TopicDiscovery`，客户端会主动移除前者。sequence 必须按连接递增；重连后可从 0 或 1 重新开始。客户端用 `agentId + topic` 构造稳定来源标识，不能只用 TCP 端点区分字段。

Windows 客户端可以启用自动恢复。传输断开、连接失败或握手超时会按 250 ms、500 ms、1 s 逐步退避，单次最长 8 s；合法 Hello 到达后退避计数复位，并在同一 host/port 上重新请求目录、恢复此前成功发送的 topic/type 订阅。手动断开会取消定时器；严格序号、非法消息等协议错误进入 Error，不自动形成错误重连循环。切换 host/port 会清空旧端点的订阅恢复集合。

## 6. ROS2 Humble 映射

第一阶段 Agent 使用 `rclcpp::NodeGraphInterface::get_topic_names_and_types()` 做发现，并针对常用消息类型做字段映射。对于运行时未知类型，Humble 的 `rclcpp::GenericSubscription` 可以接收 `rclcpp::SerializedMessage`，所以原始 CDR 转发不需要在编译期知道所有自定义消息。

任意自定义消息都必须在 Agent 环境中安装相应 ROS typesupport，`GenericSubscription` 才能创建；缺少 typesupport 时订阅失败并返回明确错误。typesupport 已安装但没有内建字段映射时，Agent 正常转发原始 CDR，字段集合为空，不能伪造解析结果。

常见带 `Header` 的消息使用 `header.stamp` 作为 source timestamp；stamp 为零或消息没有 Header 时，由服务端回退到 Agent 收到 ROS 回调的时间。结构化反序列化失败会清空字段并记录告警，但已经复制的原始 CDR 仍继续转发。

ROS graph 可以报告同名 topic 的多个类型，目录会逐类型保留。Humble RMW 不支持同一个 Agent participant 同时为同名 topic 创建不同类型的 `GenericSubscription`；Agent 会在进入 RMW 前返回明确的订阅错误，并保留此前已经成功创建的订阅。取消订阅仍按 topic/type 精确匹配。

## 7. 安全与当前边界

- 协议当前不提供认证、授权或加密，只应在受信实验室网络或 VPN/SSH 隧道中使用；
- CRC32 用于检测传输或解析错误，不是安全校验；
- 后续若直接暴露到非受信网络，应在协议外使用 TLS，并增加 Agent 访问令牌和 topic 白名单；
- 压缩标志、服务调用、参数、TF 专用消息和 rosbag2 尚未定义；未知 flags 必须忽略或显式拒绝，不能猜测含义。

## 8. 自动测试

`lab_agent_protocol_tests` 覆盖：

- CRC32 标准向量；
- 完整帧与负时间戳往返；
- 逐字节分片、粘包和垃圾前缀；
- CRC、版本、类型和超长错误后的重同步；
- Hello、HelloAck、TopicCatalog、Subscribe、SampleBatch、Error、Ping/Pong；
- 截断、尾随字节、非法布尔、非法队列深度和超长字符串拒绝。

`lab_remote_agent_tests` 使用本机 TCP 模拟 Agent，覆盖分片 Hello、能力协商、无发现能力时禁止目录请求、初始/手动目录请求、CRC 损坏恢复、TopicCatalog 与 SampleBatch 粘包、原始 CDR、数值/布尔字段、订阅/取消订阅、Ping/Pong、自动重连与订阅恢复，以及重复序号导致协议断线且不自动重试。

`lab_agent_server_session_tests` 覆盖 Agent 侧 Hello/HelloAck、能力拒绝、客户端命令动作化、服务端统一序号、时间戳、心跳、CRC 恢复、结构化错误和重复客户端序号。

`lab_agent_tcp_server_tests` 在 Linux loopback socket 上覆盖连接、并发发送严格序号、目录、订阅/取消订阅、Ping/Pong、CRC 恢复、协议断线、清理回调和重连。

ROS2 包内的 `lab_debug_agent_field_mapper_tests` 与 `lab_debug_agent_ros_integration_tests` 覆盖 Humble 序列化字段映射，以及真实 graph、reliable/best-effort QoS、GraphUpdates、原始 CDR、常见消息、未知映射类型、Header 时间戳、订阅生命周期、错误、断线清理、重连和 SIGINT。构建与运行步骤见 [`agent/ros2/lab_debug_agent/README.md`](../agent/ros2/lab_debug_agent/README.md)。
