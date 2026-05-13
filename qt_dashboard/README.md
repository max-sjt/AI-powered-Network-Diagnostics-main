# WeakNet Windows Qt Dashboard

这是给当前仓库新增的一个 Windows 可运行 Qt 桌面界面，核心目标不是替换 Linux 侧的 `DBus + eBPF` 服务，而是把项目已有能力重新组织成一个本地可视化诊断台。

当前版本已经重构成 **Qt Creator 友好的 `.ui` 工程**：

- 主窗口结构在 [qt_dashboard/src/mainwindow.ui](./src/mainwindow.ui)
- 主要行为绑定在 [qt_dashboard/src/mainwindow.cpp](./src/mainwindow.cpp)
- 趋势图使用可在 Designer 中提升的自定义控件 [qt_dashboard/src/trend_chart_widget.h](./src/trend_chart_widget.h)

## 界面包含什么

- `实时总览`
  - 自动识别本机网络接口
  - 采集默认上网接口的 RTT、丢包、RSSI、吞吐、连接数
  - 按项目服务端 `network_quality_assessor.cpp` 的思路计算综合质量
  - 用表格、趋势图和文字诊断展示结果
- `项目日志回放`
  - 兼容 `AI-assisted analysis/log_capture.py` 的输出
  - 支持按时间点查看 RTT / TCP 丢包 / RSSI / 流量 / 质量评分
  - 自动生成诊断结论
- `AI RAG 诊断`
  - 直接在 Qt 界面里调用 Python RAG 分析脚本
  - 通过 [AI-assisted analysis/qt_rag_bridge.py](../AI-assisted%20analysis/qt_rag_bridge.py) 桥接到现有 `local_vector_rag_analyzer.py`
  - 支持从“项目日志回放”页一键同步日志并按时间点做 AI 诊断
- `知识库与兼容说明`
  - 把项目功能模块、健康阈值和 Windows 兼容策略可视化说明出来

## 为什么这样做

仓库现有的 `server/` 主要依赖 Linux 的 DBus、eBPF 和相关网络能力，不能直接在 Windows 原样启动。这个 Qt 应用采用两条路径保留项目价值：

- `Windows 实时采样`
  - 用本机 WinAPI 采接口信息、ICMP RTT、WLAN RSSI、吞吐和连接数
- `项目日志回放`
  - 直接吃项目原生日志格式，回放 Linux 侧实测结果

这样既能在 Windows 真正跑起来，也不会丢掉项目已有的监控语义。

## 构建

本机已验证仓库环境可找到 `qmake`。在 Windows 下进入当前目录后运行：

```powershell
.\run_windows_gui.bat
```

如果想手动构建：

```powershell
mkdir build
cd build
qmake ..\WeakNetDashboard.pro
nmake
.\bin\WeakNetDashboard.exe
```

## 目录

- [qt_dashboard/WeakNetDashboard.pro](./WeakNetDashboard.pro)
- [qt_dashboard/src/mainwindow.ui](./src/mainwindow.ui)
- [qt_dashboard/src/mainwindow.cpp](./src/mainwindow.cpp)
- [qt_dashboard/src/windows_network_probe.cpp](./src/windows_network_probe.cpp)
- [qt_dashboard/src/project_log_parser.cpp](./src/project_log_parser.cpp)
- [AI-assisted analysis/qt_rag_bridge.py](../AI-assisted%20analysis/qt_rag_bridge.py)
