# Session 记录与回放格式

Lab Debugger 0.3 引入目录式 Session，0.4 将 TCP/UDP 配置纳入同一格式，0.8 为 Remote Agent 增加时钟质量事件，0.11 允许 rosbag2 SQLite 安全转换到同一容器，0.15 允许串口、网络和 Remote Agent 同时进入一个 Session，0.16 保存安全派生变量并在回放时恢复，0.17 为派生表达式增加确定性的滤波与时域状态，0.18 增加可回放的 Marker 与阈值告警。一次记录包含原始数据、解析结果、事件、协议快照和数据源配置，避免只保存 CSV 后无法重新分析。

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
   ├─ source_1.json ...
   ├─ derived_fields.json
   ├─ alert_rules.json
   └─ csv_fields.txt
```

## 文件职责

- `metadata.json`：格式版本、记录状态、开始/结束时间、软件/机器信息、数据源和计数；开始时写入 `recording`，正常结束后改为 `completed`。
- `raw/stream.ldraw`：RX/TX 原始字节、原始源时间、PC 接收时间、序号和 `sourceId`。这是重新解析时的权威数据。
- `values.csv`：CSV、二进制协议、ROS 映射或派生计算产生的全部数值字段，采用 17 位有效数字保存 `double`。
- `frames.jsonl`：二进制协议的逐帧结构化字段，也保留 enum、布尔和字节数组等非纯数值表现。
- `events.jsonl`：连接状态、协议错误、Session 生命周期、Remote Agent 的 `clock_sync`，以及 `marker` / `alert` 时间线事件。时钟事件消息保存 `offset_ns`、`round_trip_ns`、`uncertainty_ns` 和滚动窗口 `samples` 数量。
- `protocol/`、`configuration/`：记录开始时的协议、字段、派生表达式与数据源配置快照。

串口配置记录端口、波特率、数据位、停止位、校验和流控；网络配置记录 `tcp_client` / `tcp_server` / `udp` 模式、绑定地址、本地端口、远端地址和远端端口。Remote Agent 配置还记录自动重连开关、`ping_pong_min_rtt` 时钟估计方法和 2 秒采样间隔。开始记录时，当前所有已连接实时源都会写入 `sources` 和逐源配置文件；记录期间只允许这些来源按原配置重连，防止出现未声明来源。

`configuration/derived_fields.json` 保存名称、受限表达式与单位。开始记录时派生配置、最近值和滤波/时域状态同时冻结：历史清空后只由本 Session 已声明来源重新建立，记录期间拒绝改变表达式，迟到的未声明来源不能污染派生结果。滤波历史无需另存为不透明状态；回放以原始流和保存的表达式确定性重算。

`configuration/alert_rules.json` 保存规则名、完整字段名、高于/低于条件、有限阈值、非负回差和可选说明。记录开始时规则和激活状态冻结，记录期间拒绝修改。阈值事件采用边沿触发：持续越界只写一条 `alert`，恢复到包含回差的安全区后才允许再次触发。手动 `marker` 与实际 `alert` 结果直接保存到 `events.jsonl`；回放读取这些既成事实而不重新触发告警，因此暂停、倍速和跳转都不会生成重复事件。

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
- 派生变量配置会自动恢复并从原始数据重新计算；跳转同时清空派生最近值、滤波窗口、积分累计和微分基线，避免使用跳转前的输入。
- 告警规则和 `marker` / `alert` 时间线会自动恢复；曲线只显示当前可见时间窗内的标记，历史回放保持只读且不会重复写入事件。
- 普通 CSV/二进制回放按记录中的 `sourceId` 分别恢复半行/半帧状态；曲线字段使用“`sourceId.字段`”，同名字段不会跨来源合并。

rosbag2 导入 Session 在没有结构化采样时标记 `replay_mode: raw-only`，存在可信内置映射时标记 `replay_mode: rosbag2-structured`，并在 `configuration/rosbag2.json` 保存本次选中的 Topic/类型及 `field_mapping`。两种模式都保留选中数据的完整时间轴和原始 CDR，并明确跳过普通 CSV 与自定义二进制协议解析；结构化模式由专用 CDR 映射器按原始记录序号把数值直接送入曲线。`values.csv` 保存同一批结构化结果用于离线分析，`counts.samples`、`mapped_message_count` 和 `mapping_failure_count` 提供可核验计数。若来源带 `metadata.yaml`，其原始字节保存到 `configuration/rosbag2_metadata.yaml`，摘要、SHA-256 和 SQLite 交叉校验结果同时写入 `configuration/rosbag2.json` 与 `metadata.json.import.metadata_yaml`。单文件、分卷归并、Topic 筛选和限制见 [rosbag2 导入说明](ROSBAG2.md)。

当前索引是每条原始记录 16 字节的内存索引。后续针对数小时、极高 chunk 频率的数据，会增加后台建索引、稀疏索引和索引缓存；原始格式无需因此改变。
