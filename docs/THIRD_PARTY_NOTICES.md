# 第三方组件与 AI 使用说明（竞赛材料初稿）

更新日期：2026-09-14

本文件用于整理 Lab Debugger AI 竞赛版的主要外部组件，不代替各项目原始许可证。提交作品前应随最终便携包逐项复核实际包含的动态库，并按对应许可证保留版权、许可证文本及必要的源码获取说明。

## 运行与开发组件

| 组件 | 用途 | 是否进入 Windows 便携包 | 许可证/服务说明 |
|---|---|---|---|
| Qt 6.5.3（Core、Gui、Widgets、Network、SerialPort、Sql） | 桌面界面、串口、网络和 SQLite 驱动 | 是，动态链接 | Qt 开源版通常按 LGPLv3/GPLv3 使用；最终发布应依据所用发行包附带的许可证执行合规义务 |
| SQLite / QSQLITE | rosbag2 SQLite 读取 | QSQLITE 插件进入包 | SQLite 核心为 public domain；Qt 插件仍受 Qt 许可证约束 |
| Microsoft Visual C++ Runtime | Windows C++ 运行时 | 是 | 使用 Visual Studio 可再发行组件，遵循 Microsoft 对应再发行条款 |
| CMake | 构建与安装脚本 | 否 | 构建工具，不随应用运行包分发 |
| ROS2 Humble、rclcpp、ament | Linux Remote Agent | 不进入 Windows 包；Linux 端安装依赖 | ROS2 各包许可证以 `package.xml` 与上游仓库为准，常见核心包使用 Apache-2.0 |
| DeepSeek Chat Completions API | 竞赛版远程诊断 | 不含 SDK，仅通过 HTTPS 调用服务 | 使用参赛者自己的账户、余额和 API Key，遵守 DeepSeek 当期服务条款与隐私政策 |

项目没有把 DeepSeek API Key、模型权重或第三方训练数据打包进仓库。合成演示 Session 由项目为比赛流程原创制作并明确标注，不冒充真实机器人采集结果。

## AI 辅助开发声明建议

开发过程使用 Codex/ChatGPT 辅助需求整理、代码生成、代码审查、测试设计和文档撰写。参赛团队负责产品方向、功能取舍、代码集成、运行验证、真实实验数据与最终提交。作品运行时使用 DeepSeek 官方 API，将用户确认的本地聚合摘要转换为诊断建议；模型输出属于辅助意见，需回到曲线和设备实验复核。

正式提交时应按赛事字段列出实际使用的 AI 工具、辅助范围和人工审核责任，不应把模型生成的文案、诊断或合成数据表述为未经 AI 辅助的人工成果或真实测量事实。
