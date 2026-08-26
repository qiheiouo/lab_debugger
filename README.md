# Lab Debugger / 实验室调试助手

Lab Debugger 是面向嵌入式设备、机器人与网络设备的跨平台实时调试平台。本仓库当前实现 Phase 0、可用的 Phase 1、最小 Phase 2、Phase 3 协议引擎，以及可用的 Phase 4 Session/回放链路：

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
- 可测试的 `MockDataSource` 与核心测试。

详细设计见 [架构文档](docs/ARCHITECTURE.md)，协议格式见 [JSON 协议说明](docs/PROTOCOL_FORMAT.md)，Session 格式见 [记录与回放说明](docs/SESSION_FORMAT.md)，阶段安排见 [路线图](docs/ROADMAP.md)。

## Windows 构建

需要 Visual Studio 2022、CMake 3.24+、Qt 6.5+（Widgets 与 SerialPort）。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

运行：

```powershell
.\build\Release\LabDebugger.exe
```

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
