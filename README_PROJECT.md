# WEAK_NET 项目说明

WEAK_NET 是一个 Linux 本地弱网感知系统，适合运行在终端、网关、边缘设备或 Linux 桌面环境中。

## 系统组成

```text
weaknet-dbus-server
  -> 采集本机网络指标
  -> 写入 logs/server/runtime.log
  -> 发送 D-Bus 事件

dashboard/server.py
  -> 解析日志
  -> 读取系统网络计数器
  -> 写入 SQLite
  -> 提供 Web 页面和 API

browser
  -> 查看实时状态、趋势、事件、诊断结果
```

## 主要功能

- 当前上网网卡识别
- 默认网关识别与 RTT 探测
- 公网 RTT 探测，默认目标为 `223.5.5.5`
- TCP 重传率估算
- 网卡流量速率估算
- Wi-Fi RSSI 采集
- eBPF Top Flow 分析，失败时自动降级
- 网络质量事件
- Web Dashboard
- 中文诊断报告

## 启动

```bash
make all
```

终端 1:

```bash
./server/bin/weaknet-dbus-server
```

终端 2:

```bash
./start-dashboard.sh
```

访问:

```text
http://127.0.0.1:8080
```

局域网访问:

```bash
WEAKNET_DASHBOARD_HOST=0.0.0.0 ./start-dashboard.sh
```

## Dashboard 功能

- 实时指标卡片
- 1 秒级趋势刷新
- 点击网卡切换分析对象
- 按网卡过滤趋势、事件和诊断
- 中文 Generate 诊断结果

## 常见现象

### RSSI 显示 N/A

当前网卡不是 Wi-Fi 时正常。例如 VMware 的 `ens33` 是虚拟有线网卡，没有 RSSI。

### Traffic 一直为 0

说明当前网卡近期没有明显流量，或者 eBPF 处于降级状态。Dashboard 会使用 `/sys/class/net/<iface>/statistics` 做兜底估算。

### TCP Loss 是 0.00%

通常代表没有观察到 TCP 重传，是正常状态。

### Network Quality 仍然 POOR

当前 C++ 服务端评分逻辑仍可能受无效 RSSI/RTT 影响，后续建议优化 `network_quality_assessor.cpp`。

