# WeakNet Web Dashboard

这是 WEAK_NET 的本地 Web 看板，适合 Linux 终端、网关和边缘设备。

## 启动

先启动采集服务：

```bash
./server/bin/weaknet-dbus-server
```

再启动 Dashboard：

```bash
./start-dashboard.sh
```

打开：

```text
http://127.0.0.1:8080
```

## 页面内容

- 当前活跃网卡
- 默认网关
- RTT
- TCP Loss
- RSSI
- Traffic
- 趋势图
- 接口列表
- 事件列表
- 中文诊断结果

## 交互

- 点击任意网卡卡片可以切换分析对象
- Trend、Events、Diagnosis 会自动跟随切换

## 数据来源

Dashboard 会综合使用：

- `logs/server/runtime.log`
- SQLite 缓存 `dashboard/weaknet_dashboard.sqlite3`
- `/proc/net/route`
- `/proc/net/snmp`
- `/sys/class/net/<iface>/statistics`
- 外部 RTT 探测目标 `223.5.5.5`

## API

```text
GET /api/status
GET /api/metrics?minutes=15
GET /api/events?limit=50
GET /api/report?minutes=10
GET /api/select-interface?interface=ens33
GET /events
```

`/events` 是 SSE 流，用来推送状态更新。

## 已知说明

- RSSI 只对 Wi-Fi 网卡有效
- VMware 的 `ens33` 没有 RSSI，显示 `N/A` 正常
- 如果 eBPF 不可用，流量会降级到系统计数器估算
- 诊断结果目前以规则引擎为主，后续可接入更正式的 AI 报告

