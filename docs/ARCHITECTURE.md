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
Processing std::jthread    CSV/二进制帧解析、TimeSeries 追加、FrameEvent
Recorder std::jthread      有序写入 RX/TX 原始记录
Session std::jthread       异步写入数值、结构化帧、事件和配置快照
```

暂停终端或曲线只影响绘制，不会停止串口、处理线程或记录线程。`TimeSeriesStore` 使用共享锁保护；绘图得到有序快照，不持有内部存储引用。

当前处理和记录队列选择可靠性优先，不在压力下静默丢弃。停止 Session 时，数据源路由短暂进入屏障，等待处理队列变空后再结束记录，确保停止点前的原始数据和解析值一致。队列深度会暴露为指标；后续持续压力策略是背压告警、分块落盘和可配置内存上限，而不是无提示截断。

`ReplaySource` 实现同一个 `IDataSource` 接口，按原始接收时间调度 `DataChunk`，所以回放复用 Monitor、Protocol 和 Plot 全链路。跳转会暂停回放、等待处理队列空闲、重置流解析器后再切换索引位置。

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

`FrameStreamParser` 消费 `DataChunk`，处理固定帧头、固定/动态长度、校验与流重新同步；`ProtocolDecoder` 再按经过严格校验的 `ProtocolDefinition` 解码字段。两层分开，避免每个 STM32 协议重复实现状态机，也使 CRC 错误不会污染字段层。

JSON 是当前内建零依赖格式，加载失败时返回结构化问题且不替换运行中的协议。处理线程把数值字段写入 `TimeSeriesStore`，同时把批量 `FrameEvent` 交给 GUI Packet Inspector。热切换会丢弃模式切换前尚未解析的旧队列，原始 `.ldraw` 记录不受影响。未来可增加 YAML 前端并统一转换为同一个 `ProtocolDefinition`；表达式功能仍应使用受限 AST 和白名单函数，不执行任意脚本。

## 6. 生命周期与多数据源

应用层将演进为 `SourceManager`：管理多个 `IDataSource`、独立状态和计数，并把数据扇出到 Recorder、Parser 与 Global Timeline。当前 UI 接入一个 `SerialSource`，但核心事件和记录格式均已带 `sourceId`，没有单串口假设。

## 7. 当前已知边界

- 当前最小绘图是自绘 Qt Widget，不依赖 Qt Charts；适合 10~20 条常规曲线，但尚未实现高密度 LTTB/min-max downsampling。
- 回放索引当前在打开文件时同步建立，每条原始记录占 16 字节索引内存；超长 Session 的后台建索引与稀疏缓存仍待实现。
- CSV 解析器只处理换行分隔的数值；二进制帧、单位和校验由独立协议引擎处理。
- 串口断开后提供手动重连；自动退避重连和端口热插拔恢复留到后续。
- 时钟目前使用系统 Unix 时间。多机 ROS Agent 需要记录时钟偏移估计和同步质量。
