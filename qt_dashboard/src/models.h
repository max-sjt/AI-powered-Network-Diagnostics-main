#pragma once

#include <QtCore>
#include <QColor>

enum class NetQualityLevel {
    Unknown = 0,
    Poor = 1,
    Fair = 2,
    Good = 3,
    Excellent = 4
};

struct QualityAssessment {
    double score = 0.0;
    NetQualityLevel level = NetQualityLevel::Unknown;
    QString levelName = QStringLiteral("UNKNOWN");
    QStringList issues;
};

struct InterfaceSnapshot {
    QString name;
    QString description;
    QString ipv4;
    QString mac;
    bool isUp = false;
    bool isDefaultRoute = false;
    bool usingNow = false;
    bool isWifi = false;
    int rttMs = -1;
    double packetLossRate = -1.0;
    int rssiDbm = -1000;
    double trafficMBps = 0.0;
    quint64 packetsPerSecond = 0;
    quint32 activeConnections = 0;
    QString wifiSsid;
    QualityAssessment quality;
};

struct ProbeSnapshot {
    QDateTime capturedAt;
    QString targetHost;
    QString resolvedTarget;
    QString summary;
    QStringList timeline;
    QList<InterfaceSnapshot> interfaces;
    QualityAssessment overallQuality;
};

struct MetricRecord {
    QString timestamp;
    QString interfaceName;
    double rttMs = -1.0;
    double tcpLossRate = -1.0;
    double trafficMBps = -1.0;
    int rssiDbm = -1000;
    double qualityScore = -1.0;
    int qualityClass = -1;
    bool usingFlag = false;
    int flows = -1;
    int pps = -1;
    QString tcpLossLevel;
};

Q_DECLARE_METATYPE(ProbeSnapshot)

inline QString qualityLevelName(NetQualityLevel level) {
    switch (level) {
    case NetQualityLevel::Excellent:
        return QStringLiteral("EXCELLENT");
    case NetQualityLevel::Good:
        return QStringLiteral("GOOD");
    case NetQualityLevel::Fair:
        return QStringLiteral("FAIR");
    case NetQualityLevel::Poor:
        return QStringLiteral("POOR");
    case NetQualityLevel::Unknown:
    default:
        return QStringLiteral("UNKNOWN");
    }
}

inline QString qualityLevelLabel(NetQualityLevel level) {
    switch (level) {
    case NetQualityLevel::Excellent:
        return QStringLiteral("优秀");
    case NetQualityLevel::Good:
        return QStringLiteral("良好");
    case NetQualityLevel::Fair:
        return QStringLiteral("一般");
    case NetQualityLevel::Poor:
        return QStringLiteral("较差");
    case NetQualityLevel::Unknown:
    default:
        return QStringLiteral("未知");
    }
}

inline QColor qualityLevelColor(NetQualityLevel level) {
    switch (level) {
    case NetQualityLevel::Excellent:
        return QColor(QStringLiteral("#30c48d"));
    case NetQualityLevel::Good:
        return QColor(QStringLiteral("#57b5ff"));
    case NetQualityLevel::Fair:
        return QColor(QStringLiteral("#ffb84d"));
    case NetQualityLevel::Poor:
        return QColor(QStringLiteral("#ff6b6b"));
    case NetQualityLevel::Unknown:
    default:
        return QColor(QStringLiteral("#90a4c2"));
    }
}

inline double calculateRttScore(int rttMs) {
    if (rttMs <= 0) {
        return 50.0;
    }
    if (rttMs <= 50) {
        return 100.0;
    }
    if (rttMs <= 100) {
        return 80.0;
    }
    if (rttMs <= 200) {
        return 60.0;
    }

    const double excess = rttMs - 200;
    const double penalty = qMin(excess * 0.5, 40.0);
    return qMax(20.0, 60.0 - penalty);
}

inline double calculateLossScore(double lossRate) {
    if (lossRate < 0.0) {
        return 50.0;
    }
    if (lossRate <= 0.1) {
        return 100.0;
    }
    if (lossRate <= 0.5) {
        return 80.0;
    }
    if (lossRate <= 2.0) {
        return 60.0;
    }

    const double excess = lossRate - 2.0;
    const double penalty = qMin(excess * 20.0, 50.0);
    return qMax(10.0, 60.0 - penalty);
}

inline double calculateRssiScore(int rssiDbm) {
    if (rssiDbm <= -1000) {
        return 50.0;
    }
    if (rssiDbm >= -50) {
        return 100.0;
    }
    if (rssiDbm >= -60) {
        return 80.0;
    }
    if (rssiDbm >= -70) {
        return 60.0;
    }

    const double deficit = -70 - rssiDbm;
    const double penalty = qMin(deficit * 2.0, 50.0);
    return qMax(10.0, 60.0 - penalty);
}

inline double calculateTrafficScore(double trafficMBps, quint32 activeConnections) {
    double score = trafficMBps > 0.0 ? 70.0 : 50.0;

    if (trafficMBps >= 0.5) {
        score += 10.0;
    }
    if (trafficMBps >= 2.0) {
        score += 10.0;
    }
    if (activeConnections >= 5) {
        score += 10.0;
    }

    return qBound(0.0, score, 100.0);
}

inline QualityAssessment assessQualityMetrics(int rttMs,
                                              double lossRate,
                                              int rssiDbm,
                                              double trafficMBps,
                                              quint32 activeConnections,
                                              bool isUp) {
    QualityAssessment assessment;
    if (!isUp) {
        assessment.score = 0.0;
        assessment.level = NetQualityLevel::Poor;
        assessment.levelName = qualityLevelName(assessment.level);
        assessment.issues << QStringLiteral("接口未启用或当前不可达");
        return assessment;
    }

    const double totalScore =
        calculateRttScore(rttMs) * 0.3 +
        calculateLossScore(lossRate) * 0.3 +
        calculateRssiScore(rssiDbm) * 0.2 +
        calculateTrafficScore(trafficMBps, activeConnections) * 0.2;

    assessment.score = totalScore;
    if (totalScore >= 90.0) {
        assessment.level = NetQualityLevel::Excellent;
    } else if (totalScore >= 75.0) {
        assessment.level = NetQualityLevel::Good;
    } else if (totalScore >= 50.0) {
        assessment.level = NetQualityLevel::Fair;
    } else {
        assessment.level = NetQualityLevel::Poor;
    }
    assessment.levelName = qualityLevelName(assessment.level);

    if (rttMs > 200) {
        assessment.issues << QStringLiteral("RTT 偏高，实时应用容易卡顿");
    }
    if (lossRate > 2.0) {
        assessment.issues << QStringLiteral("TCP 丢包偏高，可能存在拥塞或链路抖动");
    }
    if (rssiDbm > -1000 && rssiDbm < -70) {
        assessment.issues << QStringLiteral("Wi-Fi 信号偏弱，建议排查距离和干扰");
    }
    if (trafficMBps <= 0.0 && activeConnections > 0) {
        assessment.issues << QStringLiteral("存在活动连接但吞吐接近 0，需关注阻塞或掉线");
    }
    if (assessment.issues.isEmpty()) {
        assessment.issues << QStringLiteral("主要指标稳定，当前未发现明显异常");
    }

    return assessment;
}
