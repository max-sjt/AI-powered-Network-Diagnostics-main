#include "windows_network_probe.h"

#include <cmath>
#include <vector>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <winsock2.h>
#include <wlanapi.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <netioapi.h>
#endif

namespace {

QString findIpv4Address(const QNetworkInterface& iface) {
    const auto entries = iface.addressEntries();
    for (const QNetworkAddressEntry& entry : entries) {
        if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol &&
            !entry.ip().isLoopback()) {
            return entry.ip().toString();
        }
    }
    return {};
}

bool interfaceLooksPhysical(const QNetworkInterface& iface) {
    if (iface.flags().testFlag(QNetworkInterface::IsLoopBack)) {
        return false;
    }
    return !iface.hardwareAddress().isEmpty();
}

} // namespace

WindowsNetworkProbe::WindowsNetworkProbe() {
#ifdef Q_OS_WIN
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
#endif
}

WindowsNetworkProbe::~WindowsNetworkProbe() {
#ifdef Q_OS_WIN
    WSACleanup();
#endif
}

QString WindowsNetworkProbe::defaultTargetHost() {
    return QStringLiteral("223.5.5.5");
}

ProbeSnapshot WindowsNetworkProbe::collect(const QString& targetHost) {
    ProbeSnapshot snapshot;
    snapshot.capturedAt = QDateTime::currentDateTime();
    snapshot.targetHost = targetHost.trimmed().isEmpty() ? defaultTargetHost() : targetHost.trimmed();

    const PingResult ping = measurePing(snapshot.targetHost);
    snapshot.resolvedTarget = ping.resolvedIp;

    const quint32 defaultIndex = resolveBestInterfaceIndex(snapshot.targetHost);
    const QHash<quint32, WifiInfo> wifiInfo = queryWifiInfo();
    const QHash<quint32, TrafficStats> trafficStats = queryTrafficStats();
    const quint32 activeConnections = estimateActiveConnections();

    QualityAssessment bestQuality;
    bestQuality.score = -1.0;

    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface& iface : interfaces) {
        if (!interfaceLooksPhysical(iface)) {
            continue;
        }

        InterfaceSnapshot item;
        item.name = iface.humanReadableName();
        item.description = iface.name();
        item.ipv4 = findIpv4Address(iface);
        item.mac = iface.hardwareAddress();
        item.isUp = iface.flags().testFlag(QNetworkInterface::IsUp) &&
                    iface.flags().testFlag(QNetworkInterface::IsRunning);
        item.isDefaultRoute = iface.index() == static_cast<int>(defaultIndex);
        item.usingNow = item.isDefaultRoute && item.isUp;

        if (wifiInfo.contains(static_cast<quint32>(iface.index()))) {
            const WifiInfo currentWifi = wifiInfo.value(static_cast<quint32>(iface.index()));
            item.isWifi = true;
            item.rssiDbm = currentWifi.rssiDbm;
            item.wifiSsid = currentWifi.ssid;
        } else {
            const QString normalizedName = item.name.toLower();
            item.isWifi = normalizedName.contains(QStringLiteral("wi-fi")) ||
                          normalizedName.contains(QStringLiteral("wifi")) ||
                          normalizedName.contains(QStringLiteral("wlan"));
        }

        if (trafficStats.contains(static_cast<quint32>(iface.index()))) {
            const TrafficStats currentTraffic = trafficStats.value(static_cast<quint32>(iface.index()));
            const CounterState previous = previousCounters_.value(static_cast<quint32>(iface.index()));
            const QDateTime now = snapshot.capturedAt;
            if (previous.capturedAt.isValid()) {
                const double seconds = qMax(0.001, previous.capturedAt.msecsTo(now) / 1000.0);
                const quint64 deltaBytes =
                    (currentTraffic.inBytes - previous.inBytes) +
                    (currentTraffic.outBytes - previous.outBytes);
                const quint64 deltaPackets =
                    (currentTraffic.inPackets - previous.inPackets) +
                    (currentTraffic.outPackets - previous.outPackets);
                item.trafficMBps = deltaBytes / seconds / 1024.0 / 1024.0;
                item.packetsPerSecond = static_cast<quint64>(deltaPackets / seconds);
            }

            previousCounters_[static_cast<quint32>(iface.index())] = {
                currentTraffic.inBytes,
                currentTraffic.outBytes,
                currentTraffic.inPackets,
                currentTraffic.outPackets,
                now
            };
        }

        if (item.usingNow) {
            item.activeConnections = activeConnections;
            if (ping.reachable) {
                item.rttMs = ping.averageRttMs;
                item.packetLossRate = ping.lossRate;
            }
        }

        item.quality = assessQualityMetrics(item.rttMs,
                                            item.packetLossRate,
                                            item.rssiDbm,
                                            item.trafficMBps,
                                            item.activeConnections,
                                            item.isUp);

        if (item.quality.score > bestQuality.score) {
            bestQuality = item.quality;
        }

        snapshot.interfaces.push_back(item);
    }

    if (snapshot.interfaces.isEmpty()) {
        snapshot.summary = QStringLiteral("未发现可用的物理网络接口");
        snapshot.overallQuality.level = NetQualityLevel::Poor;
        snapshot.overallQuality.levelName = qualityLevelName(NetQualityLevel::Poor);
        snapshot.overallQuality.issues << QStringLiteral("当前系统没有可采集的网络接口");
        snapshot.overallQuality.score = 0.0;
    } else {
        snapshot.summary = ping.reachable
            ? QStringLiteral("目标 %1 可达，已结合当前上网接口生成诊断结果").arg(snapshot.targetHost)
            : QStringLiteral("目标 %1 暂不可达，界面仍会继续展示本地接口和吞吐数据").arg(snapshot.targetHost);
        snapshot.overallQuality = bestQuality;
    }

    snapshot.timeline << snapshot.capturedAt.toString(QStringLiteral("HH:mm:ss")) + QStringLiteral(" 刷新完成");
    if (!ping.error.isEmpty()) {
        snapshot.timeline << QStringLiteral("Ping 诊断提示: %1").arg(ping.error);
    } else if (ping.reachable) {
        snapshot.timeline << QStringLiteral("Ping %1 平均 RTT %2ms，丢包 %3%")
                                 .arg(snapshot.targetHost)
                                 .arg(ping.averageRttMs)
                                 .arg(QString::number(ping.lossRate, 'f', 1));
    }
    snapshot.timeline << QStringLiteral("识别到 %1 个接口，默认路由索引 %2")
                             .arg(snapshot.interfaces.size())
                             .arg(defaultIndex);

    return snapshot;
}

QString WindowsNetworkProbe::resolveIpv4(const QString& targetHost) const {
#ifdef Q_OS_WIN
    addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    const QByteArray hostBytes = targetHost.toUtf8();
    if (getaddrinfo(hostBytes.constData(), nullptr, &hints, &result) != 0 || !result) {
        return {};
    }

    QString ip;
    for (addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
        if (ptr->ai_family == AF_INET) {
            auto* addr = reinterpret_cast<sockaddr_in*>(ptr->ai_addr);
            char buffer[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &(addr->sin_addr), buffer, sizeof(buffer));
            ip = QString::fromLatin1(buffer);
            break;
        }
    }

    freeaddrinfo(result);
    return ip;
#else
    Q_UNUSED(targetHost);
    return {};
#endif
}

WindowsNetworkProbe::PingResult WindowsNetworkProbe::measurePing(const QString& targetHost) const {
    PingResult result;
    result.resolvedIp = resolveIpv4(targetHost);
    if (result.resolvedIp.isEmpty()) {
        result.error = QStringLiteral("无法解析目标地址");
        return result;
    }

#ifdef Q_OS_WIN
    HANDLE handle = IcmpCreateFile();
    if (handle == INVALID_HANDLE_VALUE) {
        result.error = QStringLiteral("ICMP 初始化失败");
        return result;
    }

    const QByteArray payload("weaknet");
    DWORD ipAddress = inet_addr(result.resolvedIp.toLatin1().constData());
    constexpr int kAttempts = 4;
    constexpr int kTimeoutMs = 800;

    int successCount = 0;
    int totalRtt = 0;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        std::vector<char> replyBuffer(sizeof(ICMP_ECHO_REPLY) + payload.size() + 32);
        const DWORD replyCount = IcmpSendEcho(handle,
                                              ipAddress,
                                              const_cast<char*>(payload.constData()),
                                              static_cast<WORD>(payload.size()),
                                              nullptr,
                                              replyBuffer.data(),
                                              static_cast<DWORD>(replyBuffer.size()),
                                              kTimeoutMs);
        if (replyCount == 0) {
            continue;
        }

        const auto* reply = reinterpret_cast<const ICMP_ECHO_REPLY*>(replyBuffer.data());
        if (reply->Status == IP_SUCCESS) {
            ++successCount;
            totalRtt += static_cast<int>(reply->RoundTripTime);
        }
    }

    IcmpCloseHandle(handle);

    if (successCount > 0) {
        result.reachable = true;
        result.averageRttMs = qRound(totalRtt / static_cast<double>(successCount));
        result.lossRate = (kAttempts - successCount) * 100.0 / kAttempts;
    } else {
        result.error = QStringLiteral("全部 ICMP 请求超时");
    }
#else
    Q_UNUSED(targetHost);
#endif
    return result;
}

quint32 WindowsNetworkProbe::resolveBestInterfaceIndex(const QString& targetHost) const {
#ifdef Q_OS_WIN
    const QString ip = resolveIpv4(targetHost);
    if (ip.isEmpty()) {
        return 0;
    }

    const DWORD address = inet_addr(ip.toLatin1().constData());
    DWORD index = 0;
    if (GetBestInterface(address, &index) == NO_ERROR) {
        return static_cast<quint32>(index);
    }
#else
    Q_UNUSED(targetHost);
#endif
    return 0;
}

QHash<quint32, WindowsNetworkProbe::WifiInfo> WindowsNetworkProbe::queryWifiInfo() const {
    QHash<quint32, WifiInfo> wifiInfo;
#ifdef Q_OS_WIN
    HANDLE clientHandle = nullptr;
    DWORD negotiatedVersion = 0;
    if (WlanOpenHandle(2, nullptr, &negotiatedVersion, &clientHandle) != ERROR_SUCCESS) {
        return wifiInfo;
    }

    PWLAN_INTERFACE_INFO_LIST interfaceList = nullptr;
    if (WlanEnumInterfaces(clientHandle, nullptr, &interfaceList) != ERROR_SUCCESS) {
        WlanCloseHandle(clientHandle, nullptr);
        return wifiInfo;
    }

    for (unsigned int i = 0; i < interfaceList->dwNumberOfItems; ++i) {
        const WLAN_INTERFACE_INFO& info = interfaceList->InterfaceInfo[i];
        DWORD dataSize = 0;
        WLAN_OPCODE_VALUE_TYPE opcode = wlan_opcode_value_type_invalid;
        PWLAN_CONNECTION_ATTRIBUTES attributes = nullptr;

        if (WlanQueryInterface(clientHandle,
                               &info.InterfaceGuid,
                               wlan_intf_opcode_current_connection,
                               nullptr,
                               &dataSize,
                               reinterpret_cast<PVOID*>(&attributes),
                               &opcode) == ERROR_SUCCESS && attributes) {
            NET_LUID luid {};
            if (ConvertInterfaceGuidToLuid(&info.InterfaceGuid, &luid) == NO_ERROR) {
                NET_IFINDEX index = 0;
                if (ConvertInterfaceLuidToIndex(&luid, &index) == NO_ERROR) {
                    WifiInfo current;
                    current.available = true;
                    current.signalPercent = static_cast<int>(attributes->wlanAssociationAttributes.wlanSignalQuality);
                    current.rssiDbm = qRound(current.signalPercent / 2.0 - 100.0);

                    const DOT11_SSID& ssid = attributes->wlanAssociationAttributes.dot11Ssid;
                    current.ssid = QString::fromUtf8(
                        reinterpret_cast<const char*>(ssid.ucSSID),
                        static_cast<int>(ssid.uSSIDLength));
                    wifiInfo.insert(static_cast<quint32>(index), current);
                }
            }
            WlanFreeMemory(attributes);
        }
    }

    if (interfaceList) {
        WlanFreeMemory(interfaceList);
    }
    WlanCloseHandle(clientHandle, nullptr);
#endif
    return wifiInfo;
}

QHash<quint32, WindowsNetworkProbe::TrafficStats> WindowsNetworkProbe::queryTrafficStats() const {
    QHash<quint32, TrafficStats> stats;
#ifdef Q_OS_WIN
    MIB_IF_TABLE2* table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || !table) {
        return stats;
    }

    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2& row = table->Table[i];
        TrafficStats current;
        current.inBytes = row.InOctets;
        current.outBytes = row.OutOctets;
        current.inPackets = row.InUcastPkts + row.InNUcastPkts;
        current.outPackets = row.OutUcastPkts + row.OutNUcastPkts;
        stats.insert(static_cast<quint32>(row.InterfaceIndex), current);
    }

    FreeMibTable(table);
#endif
    return stats;
}

quint32 WindowsNetworkProbe::estimateActiveConnections() const {
#ifdef Q_OS_WIN
    DWORD size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (size == 0) {
        return 0;
    }

    std::vector<char> buffer(size);
    auto* table = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(buffer.data());
    if (GetExtendedTcpTable(table, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) != NO_ERROR) {
        return 0;
    }

    quint32 established = 0;
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        if (table->table[i].dwState == MIB_TCP_STATE_ESTAB) {
            ++established;
        }
    }
    return established;
#else
    return 0;
#endif
}
