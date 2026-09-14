# DeepSeek 智能诊断（2026 竞赛分支）

本页只适用于分支 `competition/deepseek-analysis-2026`。它在 Lab Debugger 0.22 的采集、解析、时间对齐、告警和 Session 回放能力之上，增加一个可现场演示的 AI 诊断闭环。比赛结束后，AI 代码默认不合并回长期主线。

## 作品定位

作品面向 ROS2 机器人、STM32 与实验室设备调试。传统工具能画曲线，但仍要求使用者人工浏览大量字段；本功能先用确定性代码从 Session 提取证据，再让大模型把证据组织为诊断假设和验证步骤。AI 不直接控制设备，也不替代原始数据。

数据流：

```text
串口 / TCP / UDP / ROS2 Topic
  → Lab Debugger 解析、时间对齐、派生、告警
  → 本地 Session
  → 本地有界摘要（字段统计 + Marker/告警）
  → 用户预览并确认
  → DeepSeek 官方 Chat Completions API
  → 证据化中文 JSON 诊断报告
```

## 使用步骤

1. 正常采集并停止一个 Session，或在“Session 回放”页打开已有 Session。
2. 打开“AI 智能诊断（比赛版）”。刚打开的回放目录会自动带入，也可以手动选择。
3. 点击“生成本地摘要”。检查待发送 JSON；它只包含字段数量、最小值、最大值、均值、标准差、首末值、时间范围和少量事件。
4. 可填写实验背景，例如设备型号、测试动作和希望判断的问题。
5. 在运行时粘贴 DeepSeek API Key。Key 只保留在本次界面的内存中，不写入 Git、配置或 Session；本竞赛版不实现系统钥匙串等复杂密钥管理。
6. 默认选择 `deepseek-v4-flash` 以降低费用；需要更强分析时可切换 `deepseek-v4-pro`。
7. 勾选发送确认，点击“发送并分析”。结果可另存为 JSON。

## API 与费用控制

实现按 2026-09-14 的 DeepSeek 官方文档调用：

- 地址：`https://api.deepseek.com/chat/completions`
- 鉴权：`Authorization: Bearer <API Key>`
- 输出：`response_format={"type":"json_object"}`
- 默认模型：`deepseek-v4-flash`
- 思考模式：关闭
- 最大输出：1400 tokens
- 超时：45 秒

程序不自动重试，避免网络异常时重复扣费。同一 Session 可反复生成本地摘要而不产生 API 费用。正式录制演示前，建议先用 Flash 完整彩排一次，并在 DeepSeek 控制台设置低余额或用量提醒。

模型名称和计费可能变化，比赛前应再次核对：

- <https://api-docs.deepseek.com/>
- <https://api-docs.deepseek.com/api/create-chat-completion/>
- <https://api-docs.deepseek.com/quick_start/pricing/>

## 摘要边界

- 最多扫描 200000 条 `values.csv` 数值行；
- 最多保留 48 个字段的聚合统计；
- 最多保留 50 条 Marker、告警或时钟事件；
- 单行最大 64 KiB；
- 最终发送摘要最大 96 KiB；
- 实验背景最多发送前 2000 个字符；
- 不读取或上传 `raw/stream.ldraw`、原始 CDR 和协议字节流。

超过上限会在摘要中标记截断或省略数量。摘要器跳过格式损坏、非有限数值或非法时间戳，不把它们伪装成有效证据。

## 报告约束

提示词要求模型输出总体判断、发现列表、证据、可能原因、验证步骤、置信度、风险和下一步行动，并明确区分事实、推断和建议。模型只能看到本地摘要，所以报告仍是辅助意见；最终结论应回到曲线、事件、原始记录和真实设备复测。

若 API 断网、超时、余额不足或返回错误，原有采集、记录和回放继续工作。现场演示应准备一个提前保存且注明生成时间的真实报告作为备用，但不能把备用报告冒充本次实时调用结果。

## 推荐比赛案例

推荐使用“ROS2 移动机器人运动异常诊断”：

- 采集 `/imu`、`/odom`、`/joint_states`、`/cmd_vel`；
- 使用统一时间轴校正 Agent 时钟；
- 添加角速度、速度差或姿态变化率派生字段；
- 用 Marker 标记直行、急转和碰撞扰动；
- 用阈值告警标记角速度或里程计漂移异常；
- 让 AI 给出可复核的证据、可能原因及下一轮验证步骤。

演示重点不是“模型什么都知道”，而是 Lab Debugger 能将跨来源真实数据压缩为有边界、可核验的证据，并让 AI 给出下一步实验方案。

## 测试范围

永久自动测试使用本机模拟 HTTP 服务，不消耗真实 API：

- Session CSV 引号解析及均值、标准差、首末值统计；
- Marker/告警提取与摘要大小限制；
- 请求地址、Bearer Header、模型、JSON 输出和关闭思考模式；
- API 响应、token 用量和结构化报告解析；
- API Key 不进入请求 JSON 正文；
- 主界面 AI 页与关键控件 smoke test。

真实 DeepSeek 联网调用需要参赛者自己的 Key，不纳入公开仓库自动测试。
