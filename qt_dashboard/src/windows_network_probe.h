#pragma once

#include "models.h"

#include <QtCore>
#include <QtNetwork>

class WindowsNetworkProbe {
public:
    WindowsNetworkProbe();
    ~WindowsNetworkProbe();

    ProbeSnapshot collect(const QString& targetHost);
    static QString defaultTargetHost();

private:
    struct PingResult {
        bool reachable = false;
        int averageRttMs = -1;
        double lossRate = -1.0;
        QString resolvedIp;
        QString error;
    };

    struct WifiInfo {
        bool available = false;
        int rssiDbm = -1000;
        int signalPercent = -1;
        QString ssid;
    };

    struct CounterState {
        quint64 inBytes = 0;
        quint64 outBytes = 0;
        quint64 inPackets = 0;
        quint64 outPackets = 0;
        QDateTime capturedAt;
    };

    struct TrafficStats {
        quint64 inBytes = 0;
        quint64 outBytes = 0;
        quint64 inPackets = 0;
        quint64 outPackets = 0;
    };

    PingResult measurePing(const QString& targetHost) const;
    QString resolveIpv4(const QString& targetHost) const;
    quint32 resolveBestInterfaceIndex(const QString& targetHost) const;
    QHash<quint32, WifiInfo> queryWifiInfo() const;
    QHash<quint32, TrafficStats> queryTrafficStats() const;
    quint32 estimateActiveConnections() const;

    QHash<quint32, CounterState> previousCounters_;
};

