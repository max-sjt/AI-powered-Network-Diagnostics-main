# AI 智能网络检测项目面经回答

## 1. 弱网项目架构介绍，背景介绍，为什么要做这样一个项目？有没有应用到实际中？

项目是一个 Linux 端弱网监控与 AI 辅助诊断平台，目标是把人工执行 `ping`、`ip route`、`ss`、`tcpdump` 等命令的排障方式，做成端侧持续监控和自动诊断。

整体分四层：

1. C++ 常驻服务 `weaknet-dbus-server`：采集网卡、默认路由、RTT、TCP 重传率、RSSI、eBPF 流量和网络质量。
2. D-Bus 通信层：注册 `com.example.WeakNet`，提供 `Get`、`Ping`、`ListInterfaces/GetInterfaces`，并推送 `Changed`、`InterfaceChanged`、`ConnectionModeChanged`、`NetworkQualityChanged`。
3. Web Dashboard：Python 解析 `logs/server/runtime.log`，结合 `/proc`、`/sys` 兜底采样，写 SQLite，并提供 `/api/status`、`/api/metrics`、`/api/events`、`/api/report`。
4. AI/RAG：基于日志指标和网络知识库，用 FAISS 检索相关知识，生成诊断建议。

当前是个人项目/MVP，没有正式生产落地，但服务端、客户端库、Dashboard、SQLite 和 RAG 分析流程都已经实现，适合继续扩展成端侧诊断 agent。

## 2. 详细描述 RTT 检测线程的功能实现和数据流动，其中存在 ICMP 头部填充的细节

RTT 线程在 `start_server()` 中启动：

```cpp
start_rtt_monitor_thread(&ctx, "223.5.5.5", 10000, 800);
```

含义是每 10 秒对 `223.5.5.5` 做一次探测，超时 800ms。数据流是：

```text
RTT线程 -> WeakNetMgr::updateRttAndStateSafe()
       -> NetPing::ping(host, iface, timeout)
       -> 更新 NetInfo.rttMs / prevRttMs / quality / state
       -> 写 RTT_MONITOR 日志
       -> 变化时通过 D-Bus emitChanged()
```

`NetPing` 使用 `socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)` 创建 raw socket，通过 `SO_BINDTODEVICE` 绑定指定网卡。ICMP 包填充 `ICMP_ECHO`、`icmp_id=getpid()`、递增 `icmp_seq`，并把 `timeval` 写入 `icmp_data`。校验和先置 0，再对 ICMP 结构计算 checksum。收到包后跳过 IP 头，校验 `ICMP_ECHOREPLY` 和 id，用返回包中的 timestamp 计算 RTT。

## 3. AI 的具体作用是什么，会不会负载很大？

AI 不参与底层实时采集，只做解释和辅助诊断。底层数据来自 C++ 服务和 Dashboard 的结构化指标，AI 根据 RTT、TCP loss、RSSI、Traffic、事件和 RAG 检索出的知识，生成“问题是什么、原因可能是什么、怎么处理”的报告。

负载不大，因为不是每秒调用模型。实时展示和基础报告由 Dashboard 规则完成，AI 更适合用户点击生成报告或指定时间点分析时调用。没有 API key 时也可以走本地规则兜底。

## 4. 传入 AI 的是什么？有多大？会不会后置，为什么不前置阈值触发 AI？

传入 AI 的不是全量日志或网页，而是结构化摘要，例如时间点、网卡名、RTT、TCP loss、RSSI、Traffic、Quality score、事件列表，再加上 RAG 检索到的知识片段，通常是 KB 级别。

AI 是后置分析，因为必须先采集到事实数据。前置阈值是应该有的，项目里 `/api/report` 已经做规则判断。更合理流程是：

```text
实时采集 -> 规则阈值判断 -> 异常摘要 -> AI/RAG 生成报告
```

这样可以减少 AI 调用成本，也避免正常状态下无意义分析。

## 5. 如何判断网络好坏？这些指标是统一标准吗？

项目综合 RTT、TCP 重传率、RSSI 和 Traffic 活跃度。当前 `NetworkQualityAssessor` 中大致是 RTT 30%、TCP loss 30%、RSSI 20%、Traffic 20%，最后映射成 `EXCELLENT/GOOD/FAIR/POOR`。

这些阈值不是强制行业标准，而是经验阈值。不同业务标准不同：游戏更关注 RTT 和抖动，视频会议关注丢包和延迟，下载关注吞吐。项目目前是规则版，后续应做场景化阈值和动态权重，尤其是有线网卡没有 RSSI 时不能让 RSSI 参与扣分。

## 6. 讲一个项目难题，怎么解决？

典型难题是异构指标时间对齐。RTT 是主动 ping，TCP loss 是两个采样点差值，Traffic 是 eBPF/sysfs 速率窗口，RSSI 来自 wpa_supplicant，它们时间不完全一致。

项目通过 `WeakNetMgr` 统一维护 `NetInfo` 状态，各线程通过线程安全方法更新；日志带时间戳，Dashboard 写入 SQLite 后按时间窗口聚合，而不是直接依赖单点。后续可以给每个指标加采样时间和有效期，评分时只使用新鲜数据。

## 7. 项目中 Wi-Fi 相关内容是什么？还了解哪些 Wi-Fi 指标？

项目实现了 RSSI 采集。RSSI 是接收信号强度，单位 dBm，越接近 0 越强。一般 `-40dBm` 很强，`-60dBm` 可用，低于 `-70dBm` 偏弱。

代码通过 Wi-Fi RSSI 模块连接 `wpa_supplicant` 控制接口读取指定 Wi-Fi 网卡信号。对 VMware 的 `ens33` 或以太网网卡，RSSI 显示 N/A 是合理的。

可扩展指标包括 SNR、噪声、信道、频段、BSSID、漫游、重连次数、协商速率、无线重传率等。

## 8. netlink 和传统 IOCTL 区别

IOCTL 更偏设备控制，接口历史包袱重，扩展性一般。netlink 是用户态和内核通信的消息机制，特别适合 Linux 网络子系统，能结构化获取和监听网卡、地址、路由、邻居表、socket 诊断等信息。

项目使用 `NETLINK_ROUTE` 监听 link/route，用 `NETLINK_SOCK_DIAG` 获取 TCP socket 诊断信息。netlink 适合常驻服务做网络状态管理。

## 9. eBPF 怎么用的？详细描述网卡流量统计

eBPF 程序在 `server/src/flow_rate.bpf.c`，定义 `BPF_MAP_TYPE_LRU_HASH`。key 是五元组：源 IP、目的 IP、源端口、目的端口、协议；value 是 bytes、packets、pid。

挂载点：

- `kprobe/ip_queue_xmit`：统计 TCP 发送。
- `kprobe/udp_sendmsg`：统计 UDP 发送。

用户态通过 libbpf 加载 `flow_rate.bpf.o`，读取 map 两次快照，计算 `delta / interval` 得到 Bps/PPS，按 Bps 排序得到 Top Flow。还基于历史窗口判断 burst、high_volume、suspicious traffic。

## 10. 弱网项目遇到的难题

主要是三类：

1. 指标时间不一致。
2. 弱网误判控制，例如 TCP loss 短窗口尖峰、RSSI 对有线无意义。
3. 环境兼容，例如 eBPF 权限、RSSI 依赖 Wi-Fi、ICMP raw socket 权限、虚拟机网卡限制。

解决方式是统一状态缓存、时间窗口聚合、无效值过滤、多指标综合判断和降级策略。

## 11. ICMP 协议实现方式

ping 基于 ICMP Echo Request/Echo Reply。项目自己构造 ICMP 包，设置 type、code、id、seq、timestamp 和 checksum，通过 raw socket 发出。收到 Echo Reply 后，跳过 IP 头，校验响应类型和 id，再根据 timestamp 算 RTT。

## 12. log 模块用什么库，同步还是异步？

日志模块封装 glog。`Logger::init()` 设置日志目录、日志大小、最低级别、stderr 输出等参数，然后调用 `google::InitGoogleLogging()`。

当前项目是直接 glog 写日志，没有自建异步日志队列。Dashboard 再 tail `runtime.log` 解析指标。后续可以改成结构化 JSON 状态接口，减少日志正则解析耦合。

## 13. 项目中怎么与内核通信？

主要方式：

- `NETLINK_ROUTE`：网卡、路由、默认出口。
- `NETLINK_SOCK_DIAG`：TCP socket 和 `tcp_info`。
- raw socket：ICMP ping。
- eBPF/libbpf：加载 BPF 程序并读取 BPF map。
- `/proc`、`/sys`：Dashboard 读取路由、TCP 统计、网卡 counters。

## 14. 怎么统计一段时间内进程发送流量？有没有模拟弱网？

eBPF map 的 value 保存 pid、bytes、packets。用户态读取 map 做两次快照：

```text
t0: 读取 bytes/packets
等待 N 秒
t1: 再读 bytes/packets
Bps = delta_bytes / N
PPS = delta_packets / N
```

因为 key 是五元组，value 有 pid，所以能定位到进程和连接。弱网可用 `tc netem` 模拟：

```bash
sudo tc qdisc add dev ens33 root netem delay 100ms loss 2%
sudo tc qdisc del dev ens33 root
```

观察 RTT、TCP loss、Dashboard 趋势和事件变化。

## 15. 项目中如何评价各个指标？

RTT 按延迟阈值评价；TCP loss 按重传率分 good/degraded/poor；RSSI 按 dBm 判断 Wi-Fi 信号；Traffic 按 Bps、PPS、active flows 和平均包大小做简单判断；最终由 `NetworkQualityAssessor` 合成 0-100 分并映射等级。

当前是规则模型，优点是可解释，缺点是固定权重对缺失指标不够友好。

## 16. eBPF map 底层用什么实现？

项目用 `BPF_MAP_TYPE_LRU_HASH`，本质是内核里的带 LRU 淘汰能力的哈希表。连接五元组数量会变化，如果不淘汰旧 flow，长期运行会占用内核内存。LRU Hash 适合这种连接级统计。

## 17. 为什么用 netlink，遇到什么难点？

用 netlink 是因为要结构化获取网卡和路由，并实时监听变化。难点是消息解析复杂，需要处理 `nlmsghdr`、`ifinfomsg`、`rtmsg`、`rtattr`，还要正确判断默认路由、IPv4/IPv6、非阻塞 socket、`NLMSG_DONE`、`NLMSG_ERROR` 等。

项目中 `UsingInterfaceManager` 通过 `NETLINK_ROUTE` 维护当前上联网卡。

## 18. 为什么自己做 ping 不用系统 ping？

自己实现能绑定指定网卡、控制超时、统一错误码、避免解析命令输出，并直接集成到 `WeakNetMgr` 状态更新中。系统 ping 适合人工排查，不适合作为常驻服务的内部采集器。

## 19. RAG 是什么？如何构建知识库？用什么向量库？

RAG 是检索增强生成。项目中 `network_knowledge_base.py` 存网络诊断知识，`local_vector_rag_analyzer.py` 把知识转成文档、切 chunk、用本地 Hash/词频 embedding 生成向量，保存到 FAISS。

分析时根据当前指标构造 query，从 FAISS 检索相关知识，再把知识和指标交给模型或本地规则生成报告。

## 20. 对 eBPF 的理解，采集网络信息挂载在哪里？

eBPF 是 Linux 内核里的安全可验证扩展机制，程序经 verifier 校验后挂到 hook 点。项目里 eBPF 用于网络发送路径观测：

- TCP 挂 `kprobe/ip_queue_xmit`
- UDP 挂 `kprobe/udp_sendmsg`

内核态只做聚合计数，用户态读取 map 做分析。

## 21. 流量统计主要挂载到什么 hook 点？

主要是两个 kprobe：`ip_queue_xmit` 和 `udp_sendmsg`。前者统计 TCP 发送路径，后者统计 UDP 发送入口。当前主要统计发送方向流量。

## 22. TCP 包丢失率怎么算？

项目算的是 TCP 重传率。通过 `NETLINK_SOCK_DIAG` 获取 TCP socket 的 `tcp_info`，累计 `tcpi_total_retrans`，估算发送段数，两次采样做差：

```text
rate = delta_retrans / delta_out * 100%
```

如果发送段数小于阈值，就标记为 `insufficient`，避免分母过小误判。

## 23. 网卡 UP/DOWN 怎么监测？网络慢除了丢包还有哪些情况？

UP/DOWN 通过 netlink 的 `RTM_NEWLINK/RTM_DELLINK` 和 `IFF_UP` 判断，但项目不会只看 UP，还结合默认路由判断是否是当前上联网卡。

网络慢还可能来自 DNS 慢、网关拥塞、Wi-Fi 干扰、上游链路拥塞、bufferbloat、NAT/防火墙慢、MTU 问题、服务端慢、本机进程占满带宽等。

## 24. 轻量级 Ping 工具如何实现？基于什么协议？

基于 ICMP Echo。流程是 raw socket、绑定网卡、解析目标、构造 ICMP Echo、写 timestamp、算 checksum、sendto、select 等待、recvfrom、跳过 IP 头、校验 Echo Reply、计算 RTT。

## 25. 两台 PC ping，从插入交换机开始发生什么？

网线插入后物理链路建立，驱动上报 link。主机获取 IP、网关、DNS。A ping B 时先查路由，判断是否同网段；同网段 ARP 查询 B 的 MAC，不同网段 ARP 查询网关 MAC。然后封装 ICMP、IP、以太网帧，经交换机转发到目标。B 回复 Echo Reply，A 收到后计算 RTT。

## 26. D-Bus 的功能和原理，为什么用它？

D-Bus 是 Linux 本机 IPC，包含 bus name、object path、interface、method、signal。项目注册 `com.example.WeakNet`，客户端可调用方法查询或 ping，也可订阅 signal。

选择 D-Bus 是因为项目是本机 agent，D-Bus 支持本机服务发现和异步事件通知，不需要暴露网络端口。HTTP 则用于 Dashboard。

## 27. 项目是分布式部署吗？Docker 如何修改？

当前是单机本地部署，不是分布式。Docker 化要处理网络命名空间、host network、`/proc`/`/sys` 挂载、D-Bus socket、eBPF 权限、`CAP_NET_RAW`、`CAP_NET_ADMIN` 等问题。若要监控宿主机，通常 agent 更适合直接部署在宿主机。

## 28. eBPF 走的是内核协议栈吗？

是的，项目挂在 `ip_queue_xmit` 和 `udp_sendmsg`，属于 Linux 内核协议栈发送路径。它不是 XDP，也不是 DPDK。如果业务绕过内核协议栈，当前 kprobe 方案就看不到完整流量。

## 29. 对内核协议栈了解吗？

应用通过 socket 发送数据后，TCP 处理连接状态、拥塞控制、重传和分段，再进入 IP 层路由、qdisc、驱动、网卡。UDP 更简单。项目 TCP 统计点在 `ip_queue_xmit`，UDP 统计点在 `udp_sendmsg`，所以定位是 Linux 内核协议栈端侧观测。

## 30. 轻量级 Ping 工具实现步骤？

创建 ICMP raw socket，绑定网卡，解析 IPv4，构造 Echo Request，填 id/seq/timestamp/checksum，发送后用 `select()` 等响应，收到后跳过 IP 头解析 Echo Reply，用 timestamp 计算 RTT。

## 31. 两台 PC ping 过程？

链路建立后获取 IP，查路由，ARP 获取下一跳 MAC，封装 ICMP/IP/以太网帧，交换机转发，目标主机协议栈处理并返回 Echo Reply，源主机计算 RTT。跨网段时经过路由器逐跳转发。

## 32. D-Bus 原理和为什么使用？

D-Bus 负责本地进程间方法调用和信号广播。项目用它给本机程序提供网络状态查询、Ping 和事件订阅。相比 HTTP，它更适合本地 agent；相比轮询，Signal 能主动通知网卡变化和网络质量变化。

## 33. 项目框架介绍，ping 工具实现原理

框架是 C++ 服务端采集指标并通过 D-Bus、日志和事件对外输出；客户端通过 `libweaknet.so` 调用；Dashboard 解析日志写 SQLite 并展示；AI/RAG 做报告。Ping 工具基于 ICMP raw socket，绑定网卡，构造 Echo 包并计算 RTT。

## 34. 项目是分布式部署吗？Docker 如何修改？

当前不是分布式，是本机采集本机展示。Docker 化时如果要看宿主机，需要 host network、挂载宿主机 `/proc` 和 `/sys`、处理 D-Bus、授予 eBPF 和 raw socket 所需 capability。分布式版本可以在每台机器部署 agent，再上报中心平台。

## 35. 为什么用 eBPF 挂载？tcp_info 不能获取吗？

`tcp_info` 可以获取 TCP 状态和重传，项目 TCP loss 也用了它。但 `tcp_info` 不适合统计所有 TCP/UDP 的连接级 bytes、packets、pid，也不覆盖 UDP。eBPF 能在内核路径低开销聚合五元组流量，适合 Top Flow 和异常流量分析。二者是互补关系。

## 36. 弱网 AI 模块怎么做的，RAG，知识库？

AI 模块在 `AI-assisted analysis/`。知识库在 `network_knowledge_base.py`，本地 RAG 在 `local_vector_rag_analyzer.py`，向量库使用 FAISS。流程是解析日志指标，构造问题，检索相关知识，拼接上下文，调用 DashScope/Qwen 或本地规则输出诊断报告。AI 不参与实时采集，只做解释和建议。
