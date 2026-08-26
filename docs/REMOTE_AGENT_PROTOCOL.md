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

当前完成的是协议和流式解码基础层；ROS2 Agent 进程、`RemoteAgentSource` 和 topic UI 仍属于后续工作。

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

字符串和字节数组采用 `uint32 length + bytes`；集合采用 `uint32 count`。字符串均为 UTF-8。

## 3. Topic 与订阅

`TopicCatalog` 包含单调变化的 `graphRevision`。每个条目保存：

- topic 完整名称；
- ROS 接口类型，例如 `sensor_msgs/msg/Imu`；
- reliability：unknown / best effort / reliable；
- durability：unknown / volatile / transient local。

Linux Agent 将使用 ROS2 Humble 的 `get_topic_names_and_types()` 建立目录。订阅请求包含独立 `requestId`，响应失败时 `Error.context` 应包含该请求或 topic。队列深度范围固定为 1..1000000。

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

## 5. 连接状态机

推荐顺序：

```text
TCP connected
  → Agent sends Hello
  → Client validates version/capabilities and sends HelloAck
  → Agent sends TopicCatalog
  → Client sends Subscribe / Unsubscribe
  → Agent streams SampleBatch and graph updates
  ↔ Ping / Pong
```

尚未完成 Hello/HelloAck 时不应接受订阅命令。sequence 必须按连接递增；重连后可从 0 或 1 重新开始。客户端用 `agentId + topic` 构造稳定来源标识，不能只用 TCP 端点区分字段。

## 6. ROS2 Humble 映射

第一阶段 Agent 使用 `rclcpp::NodeGraphInterface::get_topic_names_and_types()` 做发现，并针对常用消息类型做字段映射。对于运行时未知类型，Humble 的 `rclcpp::GenericSubscription` 可以接收 `rclcpp::SerializedMessage`，所以原始 CDR 转发不需要在编译期知道所有自定义消息。

任意自定义消息的字段树仍需要 ROS introspection typesupport。若目标机器缺少相应 typesupport，Agent 仍可转发原始 CDR 并报告“无结构化字段”，不能伪造解析结果。

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
