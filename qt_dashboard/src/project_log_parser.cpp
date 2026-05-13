#include "project_log_parser.h"

#include <QRegularExpression>
#include <algorithm>

QList<MetricRecord> ProjectLogParser::parse(const QString& logText) const {
    static const QRegularExpression rttPattern(
        QString::fromUtf8(R"(🎯 \[(\d{2}:\d{2}:\d{2})\] RTT监控: (\S+) = (\d+)ms \(质量:(\d+), 使用:(YES|NO), 目标:([^\)]+)\))"));
    static const QRegularExpression tcpPattern(
        QString::fromUtf8(R"(📈 \[(\d{2}:\d{2}:\d{2})\] TCP详细: (\S+) = ([\d.]+)% \(发送:(\d+), 重传:(\d+), 等级:(\w+)\))"));
    static const QRegularExpression trafficPattern(
        QString::fromUtf8(R"(🌊 \[(\d{2}:\d{2}:\d{2})\] 流量监控: (\S+) = ([\d.]+)MB/s \(连接:(\d+), 包/秒:(\d+)\))"));
    static const QRegularExpression rssiPattern(
        QString::fromUtf8(R"(📶 \[(\d{2}:\d{2}:\d{2})\] RSSI监控: (\S+) = (-?\d+)dBm \(质量:(\d+), 使用:(YES|NO)\))"));
    static const QRegularExpression summaryPattern(
        QString::fromUtf8(R"(📋 \[(\d{2}:\d{2}:\d{2})\] 接口汇总: (\S+) = RTT:(-?\d+)ms, 质量:(\d+), RSSI:(-?\d+)dBm, TCP丢包:(-?[\d.]+)%, 流量:([\d.]+)MB/s)"));
    static const QRegularExpression qualityPattern(
        QString::fromUtf8(R"(⭐ \[(\d{2}:\d{2}:\d{2})\] 网络质量: (\S+) = (\w+) \(分数:([\d.]+)\))"));

    QList<MetricRecord> records;
    const QStringList lines = logText.split(QRegularExpression(QStringLiteral("[\\r\\n]+")),
                                            Qt::SkipEmptyParts);

    for (const QString& line : lines) {
        auto match = rttPattern.match(line);
        if (match.hasMatch()) {
            MetricRecord record;
            record.timestamp = match.captured(1);
            record.interfaceName = match.captured(2);
            record.rttMs = match.captured(3).toDouble();
            record.qualityClass = match.captured(4).toInt();
            record.usingFlag = match.captured(5) == QStringLiteral("YES");
            records.push_back(record);
            continue;
        }

        match = tcpPattern.match(line);
        if (match.hasMatch()) {
            MetricRecord record;
            record.timestamp = match.captured(1);
            record.interfaceName = match.captured(2);
            record.tcpLossRate = match.captured(3).toDouble();
            record.tcpLossLevel = match.captured(6);
            records.push_back(record);
            continue;
        }

        match = trafficPattern.match(line);
        if (match.hasMatch()) {
            MetricRecord record;
            record.timestamp = match.captured(1);
            record.interfaceName = match.captured(2);
            record.trafficMBps = match.captured(3).toDouble();
            record.flows = match.captured(4).toInt();
            record.pps = match.captured(5).toInt();
            records.push_back(record);
            continue;
        }

        match = rssiPattern.match(line);
        if (match.hasMatch()) {
            MetricRecord record;
            record.timestamp = match.captured(1);
            record.interfaceName = match.captured(2);
            record.rssiDbm = match.captured(3).toInt();
            record.qualityClass = match.captured(4).toInt();
            record.usingFlag = match.captured(5) == QStringLiteral("YES");
            records.push_back(record);
            continue;
        }

        match = summaryPattern.match(line);
        if (match.hasMatch()) {
            MetricRecord record;
            record.timestamp = match.captured(1);
            record.interfaceName = match.captured(2);
            record.rttMs = match.captured(3).toDouble();
            record.qualityClass = match.captured(4).toInt();
            record.rssiDbm = match.captured(5).toInt();
            record.tcpLossRate = match.captured(6).toDouble();
            record.trafficMBps = match.captured(7).toDouble();
            if (record.rttMs < 0.0) {
                record.rttMs = -1.0;
            }
            if (record.rssiDbm <= -1000) {
                record.rssiDbm = -1000;
            }
            if (record.tcpLossRate < 0.0) {
                record.tcpLossRate = -1.0;
            }
            records.push_back(record);
            continue;
        }

        match = qualityPattern.match(line);
        if (match.hasMatch()) {
            MetricRecord record;
            record.timestamp = match.captured(1);
            record.interfaceName = match.captured(2);
            record.qualityScore = match.captured(4).toDouble();
            records.push_back(record);
        }
    }

    return records;
}

QStringList ProjectLogParser::availableTimes(const QList<MetricRecord>& records) const {
    QSet<QString> uniqueTimes;
    for (const MetricRecord& record : records) {
        uniqueTimes.insert(record.timestamp);
    }

    QStringList times = uniqueTimes.values();
    std::sort(times.begin(), times.end());
    return times;
}

QList<MetricRecord> ProjectLogParser::recordsForTime(const QList<MetricRecord>& records, const QString& timestamp) const {
    QList<MetricRecord> result;
    for (const MetricRecord& record : records) {
        if (record.timestamp == timestamp) {
            result.push_back(record);
        }
    }
    return result;
}

QString ProjectLogParser::sampleLog() const {
    return QString::fromUtf8(R"(🎯 [00:13:24] RTT监控: eth0 = 15ms (质量:1, 使用:YES, 目标:223.5.5.5)
📈 [00:13:24] TCP详细: eth0 = 0.5% (发送:137, 重传:0, 等级:good)
🌊 [00:13:24] 流量监控: eth0 = 2.5MB/s (连接:15, 包/秒:1200)
📶 [00:13:24] RSSI监控: wlan0 = -65dBm (质量:2, 使用:NO)
📋 [00:13:24] 接口汇总: eth0 = RTT:15ms, 质量:1, RSSI:-1000dBm, TCP丢包:0.5%, 流量:2.5MB/s
⭐ [00:13:30] 网络质量: eth0 = good (分数:85.5)
🎯 [00:13:50] RTT监控: eth0 = 18ms (质量:1, 使用:YES, 目标:223.5.5.5)
📈 [00:13:50] TCP详细: eth0 = 0.8% (发送:145, 重传:1, 等级:good)
🌊 [00:13:50] 流量监控: eth0 = 3.2MB/s (连接:18, 包/秒:1500)
📋 [00:13:50] 接口汇总: eth0 = RTT:18ms, 质量:1, RSSI:-1000dBm, TCP丢包:0.8%, 流量:3.2MB/s
⭐ [00:13:50] 网络质量: eth0 = good (分数:82.0)
🎯 [00:14:12] RTT监控: wlan0 = 82ms (质量:2, 使用:YES, 目标:223.5.5.5)
📈 [00:14:12] TCP详细: wlan0 = 2.6% (发送:125, 重传:4, 等级:poor)
🌊 [00:14:12] 流量监控: wlan0 = 0.9MB/s (连接:7, 包/秒:620)
📶 [00:14:12] RSSI监控: wlan0 = -74dBm (质量:3, 使用:YES)
📋 [00:14:12] 接口汇总: wlan0 = RTT:82ms, 质量:3, RSSI:-74dBm, TCP丢包:2.6%, 流量:0.9MB/s
⭐ [00:14:12] 网络质量: wlan0 = fair (分数:58.0)
)");
}
