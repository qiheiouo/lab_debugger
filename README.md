# Lab Debugger / 实验室调试助手

Lab Debugger 是面向嵌入式设备、机器人与网络设备的跨平台实时调试平台。本仓库当前实现 Phase 0、可用的 Phase 1、最小 Phase 2、Phase 3 协议引擎、Phase 4 Session/回放链路、Phase 5 网络数据源、Phase 6 Remote ROS Agent 主链路，并已进入 Phase 7 rosbag2 离线分析：

- 与 Qt UI 解耦的 C++20 数据核心；
- 多数据源友好的 `IDataSource` 抽象；
- 独立线程的串口 I/O、文本解析和原始数据写盘；
- Windows 串口枚举、连接、收发、ASCII/HEX、定时发送；
- RX/TX 时间戳终端、暂停显示、保存、复制；
- CSV 风格数值流解析、时序环形缓冲、统计与实时曲线；
- 严格 JSON 二进制协议描述、流式帧同步、字段解码和 CRC；
- 协议加载界面、逐帧检查器、错误统计，以及数值字段自动接入曲线；
- 目录式 Session：原始流、数值、结构化帧、事件、元数据和配置快照；
- 正式 `ReplaySource`：0.1×~10×、暂停/继续、跳转、截断尾部恢复；
- 独立 I/O 线程的 TCP 客户端、单连接 TCP 服务端和 UDP 收发；
- 网络数据复用终端、协议、曲线、记录与回放全链路；
- 无 Qt/ROS 依赖的 Remote Agent 帧、握手、topic、订阅和样本编解码；
- Remote Agent CRC32、长度防护、流式分片/粘包处理与错误重同步；
- 独立 TCP 线程的 `RemoteAgentSource`、5 秒握手超时、严格序号检查与 Ping/Pong；
- 可选指数退避自动重连、同端点订阅恢复与手动断开取消重试；
- 基于 Ping/Pong 的 Agent 时钟偏移、RTT 与不确定度估计，结果显示并写入 Session 事件；
- ROS Agent 身份、Topic 目录、订阅/取消订阅 UI，以及 CDR/结构化字段双路记录；
- Remote Agent 本机模拟服务器回环测试；
- 已在 Ubuntu 22.04/ROS2 Humble + GCC 11 验证的 `lab_debug_agent` ament 包、单客户端 TCP 服务端和 graph 更新；
- 基于 `GenericSubscription` 的任意类型 CDR 转发、常见消息语义映射，以及自定义消息的运行时 introspection 字段展开；
- 后台 rosbag2 SQLite 预检/导入、可搜索 Topic 筛选、分卷时间归并和原始 CDR 安全回放；
- 可测试的 `MockDataSource` 与核心测试。

详细设计见 [架构文档](docs/ARCHITECTURE.md)，协议格式见 [JSON 协议说明](docs/PROTOCOL_FORMAT.md)，Session 格式见 [记录与回放说明](docs/SESSION_FORMAT.md)，网络语义见 [TCP/UDP 使用说明](docs/NETWORK.md)，远程 ROS 协议见 [Remote Agent 协议](docs/REMOTE_AGENT_PROTOCOL.md)，rosbag2 使用与边界见 [rosbag2 导入说明](docs/ROSBAG2.md)，阶段安排见 [路线图](docs/ROADMAP.md)。

## Windows 构建

需要 Visual Studio 2022、CMake 3.24+、Qt 6.5+（Widgets、SerialPort、Network 与 Sql/QSQLITE）。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix dist/LabDebugger
```

安装步骤会把所需 Qt DLL 和平台插件部署到 `dist/LabDebugger`，推荐运行部署后的版本：

```powershell
.\dist\LabDebugger\bin\LabDebugger.exe
```

若只从 Qt/Visual Studio 开发终端调试，也可以直接运行 `build\Release\LabDebugger.exe`；普通终端直接运行构建目录版本时，需要确保对应 Qt `bin` 位于 `PATH`，避免误加载系统中其他软件附带的 Qt DLL。

若没有 Qt，可用 `-DLAB_DEBUGGER_BUILD_GUI=OFF` 单独构建核心和测试。

## STM32 快速验证

1. 用 USB-UART 连接 STM32，选择对应 COM 口与波特率。
2. 点击“连接”，在“终端”页选择 ASCII 或 HEX 观察收发。
3. STM32 以换行结尾发送 `1.24,3.56,23.80\n`。
4. 在“实时曲线”页把字段设为 `speed,current,voltage` 并点击“应用”。
5. 勾选需要观察的字段；曲线以 30 FPS 刷新，采集和解析仍按原始速率进行。

若 STM32 输出二进制帧，打开“协议解析”页并加载
`examples/protocols/stm32_status.json`。有效帧、丢弃字节和校验错误会分别统计，
最近一帧的字段与原始 HEX 会显示在检查器中，所有数值字段会自动出现在实时曲线页。

## 原始记录格式

工具栏点击“开始 Session 记录”，选择父目录后会创建带时间戳的 Session 文件夹。点击停止时，应用先等待已接收数据完成解析，再安全结束原始流和解析结果，避免尾部不一致。

打开“Session 回放”页选择历史目录即可回放。回放默认暂停，数据进入与实时串口完全相同的终端、协议和曲线链路；可选择倍速并拖动时间轴跳转。格式与恢复规则见 [记录与回放说明](docs/SESSION_FORMAT.md)。

同一页面可直接导入单个 rosbag2 `.db3` 文件或包含多个分卷的 bag 目录。程序先在后台合并 Topic 目录，再提供搜索、消息计数和选择窗口，正式任务只写入选中 Topic；成功后自动打开标准 Session。ROS CDR 只进入原始终端和回放，不会被当作 CSV。详细规则见 [rosbag2 导入说明](docs/ROSBAG2.md)。

## TCP/UDP 快速验证

左侧切换到“网络”页后选择模式：

- TCP 客户端填写远端主机和端口；连接成功后即可双向收发字节流；
- TCP 服务端填写监听地址和本地端口；当前保留一个活动客户端，新连接会替换旧连接；
- UDP 同时填写本地绑定地址/端口和远端数字 IP/端口；每个收到的数据报形成一个独立数据块。

网络 RX/TX 会进入和串口相同的终端、CSV/二进制协议、曲线及 Session。更完整的模式语义和限制见 [TCP/UDP 使用说明](docs/NETWORK.md)。

## Remote ROS Agent 客户端

左侧切换到“ROS Agent”页，填写 Linux Agent 地址和端口后连接。客户端必须在 5 秒内收到合法 `Hello`，随后才会显示为就绪；Agent 提供发现能力时客户端自动请求 Topic 目录。新版 Agent 还会为每个 topic/type 显示“内置语义、通用解析、仅原始 CDR、不可订阅”及具体原因，旧 Agent 则明确显示“未提供”。选择 Topic、可靠性与队列深度后可订阅或取消订阅；收到的原始 CDR 进入终端和 Session 原始流，数值及布尔字段直接进入实时曲线和 `values.csv`。客户端还会每 2 秒通过 Ping/Pong 更新 Agent 时钟偏移、RTT 与不确定度，连接页显示当前质量，记录期间写入 `clock_sync` 事件。启用自动重连后，临时网络故障按 250 ms 至 8 s 指数退避，同一端点重新握手成功后会刷新目录并恢复已订阅的 topic/type；手动断开会立即取消重试，协议错误不会无限重连。

Linux/ROS2 Humble Agent 位于 [`agent/ros2/lab_debug_agent`](agent/ros2/lab_debug_agent/README.md)。Agent 对内置常见消息保留带单位的语义映射，对其余已安装 C++ 与 introspection typesupport 的消息递归展开标量、字符串、嵌套成员和数组；无法安全结构化的内容仍保留原始 CDR。ament 包此前已在 Ubuntu 22.04.5 + ROS2 Humble + GCC 11.4 环境完成构建、launch、真实 ROS graph/CDR、QoS、订阅生命周期、错误恢复、断线重连和 SIGINT 自动测试。协议暂不提供认证或加密，推荐让 Agent 只监听回环地址并通过 SSH 隧道连接。
