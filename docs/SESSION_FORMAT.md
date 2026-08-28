# Session 记录与回放格式

Lab Debugger 0.3 引入目录式 Session，0.4 将 TCP/UDP 配置纳入同一格式，0.8 为 Remote Agent 增加时钟质量事件，0.11 允许 rosbag2 SQLite 安全转换到同一容器。一次记录包含原始数据、解析结果、事件、协议快照和数据源配置，避免只保存 CSV 后无法重新分析。

```text
session_YYYYMMDD_HHMMSS/
├─ metadata.json
├─ raw/
│  └─ stream.ldraw
├─ values.csv
├─ frames.jsonl
├─ events.jsonl
├─ protocol/
│  ├─ initial.json
│  └─ <timestamp>_<name>.json
└─ configuration/
   ├─ source_0.json
   └─ csv_fields.txt
```

## 文件职责

- `metadata.json`：格式版本、记录状态、开始/结束时间、软件/机器信息、数据源和计数；开始时写入 `recording`，正常结束后改为 `completed`。
- `raw/stream.ldraw`：RX/TX 原始字节、原始源时间、PC 接收时间、序号和 `sourceId`。这是重新解析时的权威数据。
- `values.csv`：CSV 或二进制协议产生的全部数值字段，采用 17 位有效数字保存 `double`。
- `frames.jsonl`：二进制协议的逐帧结构化字段，也保留 enum、布尔和字节数组等非纯数值表现。
- `events.jsonl`：连接状态、协议错误、Session 生命周期事件，以及 Remote Agent 的 `clock_sync` 事件。时钟事件消息保存 `offset_ns`、`round_trip_ns`、`uncertainty_ns` 和滚动窗口 `samples` 数量。
- `protocol/`、`configuration/`：记录开始时的协议与字段/数据源配置快照。

串口配置记录端口、波特率、数据位、停止位、校验和流控；网络配置记录 `tcp_client` / `tcp_server` / `udp` 模式、绑定地址、本地端口、远端地址和远端端口。Remote Agent 配置还记录自动重连开关、`ping_pong_min_rtt` 时钟估计方法和 2 秒采样间隔。

## 原始流格式

所有多字节整数明确使用小端编码，不依赖运行平台本机端序。

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

索引器会验证文件头、记录标记、方向和长度安全上限。程序或机器意外中止造成尾部记录不完整时，完整前缀仍可回放并明确显示“已恢复截断尾部”；中间记录损坏则拒绝打开，避免悄悄产生错误时间线。

## 回放语义

`ReplaySource` 是正式 `IDataSource`：终端、协议解析、TimeSeries 和曲线不会区分数据来自实时串口还是历史文件。

- 调度时间轴使用原始 `receiveTimestamp`，文件内小幅倒退会被夹到非递减时间；
- `sourceTimestamp`、`receiveTimestamp`、方向、序号和来源均原样交给上层；
- 支持 `0.1× / 0.5× / 1× / 2× / 5× / 10×`、暂停、继续和跳转；
- 跳转时先建立处理屏障，再清理半帧/半行缓存和旧曲线，避免把跳转前后的字节拼成伪帧；
- Session 的初始协议或 CSV 字段配置会自动恢复。

rosbag2 导入 Session 额外标记 `replay_mode: raw-only`，并在 `configuration/rosbag2.json` 保存本次选中的 Topic/类型目录。此模式保留选中数据的完整时间轴和原始 CDR，但明确跳过 CSV 与自定义二进制协议解析，避免随机二进制形成伪数值或伪帧。单文件、分卷归并、Topic 筛选和限制见 [rosbag2 导入说明](ROSBAG2.md)。

当前索引是每条原始记录 16 字节的内存索引。后续针对数小时、极高 chunk 频率的数据，会增加后台建索引、稀疏索引和索引缓存；原始格式无需因此改变。
