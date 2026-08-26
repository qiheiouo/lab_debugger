# Lab Debugger / 实验室调试助手

Lab Debugger 是面向嵌入式设备、机器人与网络设备的跨平台实时调试平台。本仓库当前实现 Phase 0、可用的 Phase 1、最小 Phase 2，以及 Phase 3 协议核心：

- 与 Qt UI 解耦的 C++20 数据核心；
- 多数据源友好的 `IDataSource` 抽象；
- 独立线程的串口 I/O、文本解析和原始数据写盘；
- Windows 串口枚举、连接、收发、ASCII/HEX、定时发送；
- RX/TX 时间戳终端、暂停显示、保存、复制；
- CSV 风格数值流解析、时序环形缓冲、统计与实时曲线；
- 严格 JSON 二进制协议描述、流式帧同步、字段解码和 CRC；
- 可测试的 `MockDataSource` 与核心测试。

详细设计见 [架构文档](docs/ARCHITECTURE.md)，协议格式见 [JSON 协议说明](docs/PROTOCOL_FORMAT.md)，阶段安排见 [路线图](docs/ROADMAP.md)。

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

## 原始记录格式

工具栏“开始原始记录”产生 `.ldraw` 文件。文件保存 RX/TX 原始字节、方向、源时间、接收时间与序号；解析器以后修复时仍能从原始数据重新分析。格式细节见架构文档。
