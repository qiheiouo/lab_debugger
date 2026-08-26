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

## 已实现核心：Phase 3 — Protocol Engine

- 不可变 `ProtocolDefinition` 与严格 JSON 加载；
- 流式 frame synchronizer：半帧、粘包、垃圾字节、错误长度、CRC 错误和重同步；
- 整数、浮点、布尔、字节数组、端序、scale、offset、unit、enum；
- sum8、CRC8/ATM、CRC16/MODBUS、CRC16/CCITT-FALSE；
- 固定长度与 `uint8/16/32` 动态长度字段；
- 首字节时间戳保留和解析统计。

尚需完成：把协议核心接入 GUI、Packet Inspector、协议文件热切换，以及解析字段自动进入实时曲线。

## 后续顺序

- Phase 4：完整 Session、索引、ReplaySource、seek/speed/pause；
- Phase 5：TCP/UDP adapters；
- Phase 6：Linux ROS2 adapter 与远程 Agent；
- Phase 7：generic ROS introspection 与 rosbag2；
- Phase 8：派生字段、滤波、marker、告警、多源同步分析。

每阶段必须先补数据格式、线程与错误路径测试，再扩 UI。性能验证重点是 921600 baud、1000 Hz 数值流、数小时记录和 UI 暂停期间的数据完整性。
