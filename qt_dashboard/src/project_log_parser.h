#pragma once

#include "models.h"

#include <QtCore>

class ProjectLogParser {
public:
    QList<MetricRecord> parse(const QString& logText) const;
    QStringList availableTimes(const QList<MetricRecord>& records) const;
    QList<MetricRecord> recordsForTime(const QList<MetricRecord>& records, const QString& timestamp) const;
    QString sampleLog() const;
};

