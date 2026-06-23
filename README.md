# WEAK_NET AI-powered Network Diagnostics

WEAK_NET 是一个面向 Linux 的本地弱网监控与诊断项目。
它由 C++ 常驻采集服务、C API 客户端库、命令行测试工具、Web Dashboard 和 AI 辅助分析模块组成。

## 现在能做什么

- 监控当前上网网卡
- 采集 RTT、TCP 重传/丢包、Wi-Fi RSSI、流量、质量评分
- 通过 D-Bus 暴露接口和事件
- 用 Web 页面查看实时状态、趋势、事件和诊断结果
- 对日志/结构化数据做本地规则诊断和 AI 辅助分析

## 目录

- `server/` C++ 采集服务端
- `client/` C/C++ 客户端库和测试工具
- `dashboard/` 本地 Web Dashboard
- `AI-assisted analysis/` AI 辅助分析和 RAG 工具
- `logs/server/` 服务端日志

## 快速开始

### 1. 安装依赖

Ubuntu / Debian:

```bash
sudo apt-get update
sudo apt-get install -y build-essential clang llvm pkg-config libdbus-1-dev libglog-dev libelf-dev zlib1g-dev libcap-dev linux-headers-$(uname -r) libbpf-dev
```

### 2. 编译

```bash
make all
```

### 3. 启动采集服务

```bash
./server/bin/weaknet-dbus-server
```

### 4. 启动 Web Dashboard

另开一个终端:

```bash
./start-dashboard.sh
```

浏览器打开:

```text
http://127.0.0.1:8080
```

## Web Dashboard

Dashboard 会显示:

- 当前活跃网卡
- 默认网关
- RTT
- TCP Loss
- RSSI
- Traffic
- 趋势图
- 事件列表
- 中文诊断结果

### 交互

- 点击某个网卡，可以切换当前分析对象
- Trend、Events、Diagnosis 会跟着切换

## API 与数据来源

Dashboard 使用以下数据源:

- `logs/server/runtime.log`
- SQLite 缓存 `dashboard/weaknet_dashboard.sqlite3`
- 系统计数器 `/sys/class/net/*/statistics`
- TCP 统计 `/proc/net/snmp`
- 默认网关 `/proc/net/route`

主要 API:

```text
GET /api/status
GET /api/metrics?minutes=15
GET /api/events?limit=50
GET /api/report?minutes=10
GET /api/select-interface?interface=ens33
GET /events
```

## 客户端

客户端库位于 `client/`，提供:

- `libweaknet.so`
- `client/bin/test-client`
- C API 头文件 `client/weaknet_client.h`

常用测试:

```bash
make test-client COMMAND=get
make test-client COMMAND=health
make test-client COMMAND=ping google.com
```

## AI 辅助分析

`AI-assisted analysis/` 目录提供日志解析、本地 RAG 和 AI 分析入口。
现在更推荐用 Dashboard 生成的结构化数据做分析，而不是直接解析网页。

## 已知边界

- RSSI 只对 Wi-Fi 接口有意义
- VMware / 有线网卡环境下 RSSI 可能为 `N/A`
- eBPF 流量分析在权限不足时会降级
- 质量分数目前仍偏保守，后续建议进一步把无效值从评分中排除

## 文档

- [Dashboard README](dashboard/README.md)
- [客户端 API](client/README_CLIENT.md)
- [客户端库](client/README_LIBRARY.md)
- [事件系统](README_EVENTS.md)
- [AI 分析](AI-assisted%20analysis/README.md)

