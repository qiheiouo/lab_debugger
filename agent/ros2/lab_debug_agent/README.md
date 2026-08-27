# Lab Debug Agent（ROS2 Humble）

该 ament 包运行在 Ubuntu 22.04 / ROS2 Humble 机器人主机上，把 ROS graph 和订阅数据通过 Lab Debugger Remote Agent v1 协议发送给桌面客户端。Windows 端不需要安装 ROS2。

## 功能

- 发现 topic、消息类型与发布端 QoS，并维护单调递增的 graph revision；
- 使用 `rclcpp::GenericSubscription` 接收运行时指定类型的序列化消息；
- 所有已订阅消息都保留原始 CDR；
- 为以下常用类型额外生成结构化字段：
  - `std_msgs/msg/Bool`、`Float32`、`Float64`、`Int32`、`UInt32`、`String`；
  - `geometry_msgs/msg/Twist`、`TwistStamped`、`PoseStamped`；
  - `sensor_msgs/msg/Imu`、`JointState`；
  - `nav_msgs/msg/Odometry`；
- 支持目录主动刷新、graph 更新推送、订阅/取消订阅、协议错误和 Ping/Pong；
- 单客户端 TCP 会话，客户端断开时自动释放其 ROS 订阅。

未知消息类型仍会正常转发 CDR，只是暂时没有结构化曲线字段。自定义消息的类型支持库必须已经安装，并且其工作空间已在启动 Agent 前 `source`；否则 `GenericSubscription` 会返回明确错误。

## 构建

保持本仓库目录结构不变，因为 ament 包会复用仓库 `src/core` 中的协议实现：

```bash
cd ~/lab_debugger
source /opt/ros/humble/setup.bash
colcon build \
  --base-paths agent/ros2 \
  --packages-select lab_debug_agent \
  --symlink-install
source install/setup.bash
```

运行包内的字段映射和真实 ROS/DDS/TCP 联调测试：

```bash
colcon test \
  --base-paths agent/ros2 \
  --packages-select lab_debug_agent \
  --event-handlers console_direct+
colcon test-result --test-result-base build/lab_debug_agent --verbose
```

测试会启动只监听随机回环端口的 Agent 子进程，创建 Float64、Bool、String、Twist、TwistStamped、Imu、JointState、Odometry、未知映射 Point 和同名多类型等真实 ROS graph 端点，并验证协议数据而非只检查日志。

若需要订阅自定义消息，应先 source 对应工作空间，再构建和运行：

```bash
source ~/robot_ws/install/setup.bash
source ~/lab_debugger/install/setup.bash
```

## 运行

最安全的默认方式只监听回环地址：

```bash
ros2 launch lab_debug_agent agent.launch.py \
  bind_address:=127.0.0.1 port:=9750 agent_id:=robot-main
```

然后从 Windows 建立 SSH 隧道：

```powershell
ssh -L 9750:127.0.0.1:9750 user@robot-host
```

Lab Debugger 中填写 `127.0.0.1:9750`。若实验室受信局域网确实需要直接连接，可设置 `bind_address:=0.0.0.0`，并只对 Windows 调试机 IP 放行端口。协议目前没有认证或 TLS，不应把端口暴露到互联网或不可信网络。

## 参数

| 参数 | 默认值 | 说明 |
|---|---|---|
| `bind_address` | `127.0.0.1` | 数字 IPv4 监听地址 |
| `port` | `9750` | TCP 端口，范围 1..65535 |
| `agent_id` | `lab-agent` | 客户端显示和 `sourceId` 使用的稳定 Agent 标识 |

## 当前边界

- 只允许一个活动 Lab Debugger 客户端；
- 订阅可靠性按客户端请求设置；未知可靠性使用 best effort，以兼容 best-effort 与 reliable 发布端；
- 订阅持久性当前固定为 volatile，因此不会补收 transient-local 历史样本；
- 常见消息字段映射使用已编译类型，其他类型只转发 CDR；通用 introspection 字段树仍待实现；
- 活动订阅按 topic/type 区分；相同 QoS 的重复请求幂等，不同 QoS 会替换该 topic/type，取消订阅不会误删同名其他类型；
- ROS graph 可列出同名多类型，但 Humble RMW 不支持在同一 Agent participant 内同时创建不同类型的 GenericSubscription；Agent 会在进入 RMW 前返回清晰错误并保留已有订阅；
- 已在 Ubuntu 22.04.5 + ROS2 Humble + GCC 11.4 下完成构建、launch、真实 graph/CDR、字段、QoS、错误恢复、重连与 SIGINT 自动验证。
