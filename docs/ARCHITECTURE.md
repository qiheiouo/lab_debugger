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
                         ▼            ▼
                    DataSample     Terminal
                         │
                         ▼
                  TimeSeriesStore
                         │ snapshot
                         ▼
                       Plot
```

`IDataSource` 只表达打开、关闭、状态、写入、数据、错误与统计。上层从 `DataChunk` 中得到来源、方向、源时间、接收时间、序号和字节，不需要知道数据来自 COM、UDP 还是 ROS2。

核心库不用 Qt 类型，便于独立测试、用于无界面 Agent，或未来抽成基础设施库。Qt 目前只出现在串口适配器和桌面界面。

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

多字节整数当前以小端写入；正式 Session 格式阶段会加入版本、端序标记、校验与索引。

## 3. 线程模型

```text
GUI thread                 只处理交互、33 ms 批量终端刷新和绘图快照
Serial QThread             QSerialPort 的 open/read/write/error 生命周期
Processing std::jthread    字节分行、CSV 字段解析、TimeSeries 追加
Recorder std::jthread      有序写入 RX/TX 原始记录
```

暂停终端或曲线只影响绘制，不会停止串口、处理线程或记录线程。`TimeSeriesStore` 使用共享锁保护；绘图得到有序快照，不持有内部存储引用。

当前处理和记录队列选择可靠性优先，不在压力下静默丢弃。队列深度会暴露为指标；后续持续压力策略是背压告警、分块落盘和可配置内存上限，而不是无提示截断。

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

远程帧必须携带 topic、message type、源时间、Agent 接收时间、序号与负载。第一版 Agent 先支持常用消息的结构化字段；之后再增加 GenericSubscription、类型描述与 CDR introspection。ROS2 构建通过独立 CMake 选项和目标启用，不向核心传播头文件或链接依赖。

## 5. 协议引擎边界

后续 `IStreamDecoder` 消费 `DataChunk` 并产生 `Frame`；`IFieldDecoder` 按 JSON/YAML 描述把 `Frame` 变成 `DataSample`。帧同步、长度、CRC 与字段解码分层，避免每个 STM32 协议写一套硬编码状态机。

推荐协议描述先选 JSON 作为内建零依赖格式，再增加 YAML 前端；加载后统一编译为经过边界检查的不可变 `ProtocolDefinition`。表达式引擎使用受限 AST 和白名单函数，不执行任意脚本。

## 6. 生命周期与多数据源

应用层将演进为 `SourceManager`：管理多个 `IDataSource`、独立状态和计数，并把数据扇出到 Recorder、Parser 与 Global Timeline。当前 UI 接入一个 `SerialSource`，但核心事件和记录格式均已带 `sourceId`，没有单串口假设。

## 7. 当前已知边界

- 当前最小绘图是自绘 Qt Widget，不依赖 Qt Charts；适合 10~20 条常规曲线，但尚未实现高密度 LTTB/min-max downsampling。
- `.ldraw` 是 Phase 1 原始流格式，不等同于 Phase 4 完整 Session。
- CSV 解析器只处理换行分隔的数值，不处理二进制帧、单位和校验。
- 串口断开后提供手动重连；自动退避重连和端口热插拔恢复留到后续。
- 时钟目前使用系统 Unix 时间。多机 ROS Agent 需要记录时钟偏移估计和同步质量。

