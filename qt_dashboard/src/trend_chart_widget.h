#pragma once

#include <QColor>
#include <QWidget>

class TrendChartWidget : public QWidget {
    Q_OBJECT

public:
    explicit TrendChartWidget(QWidget* parent = nullptr);
    TrendChartWidget(const QString& title, const QColor& accent, QWidget* parent = nullptr);

    void setSeries(const QVector<double>& series, const QString& unit, const QString& footer = QString());
    void setEmptyText(const QString& text);
    void setPresentation(const QString& title, const QColor& accent);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString title_;
    QColor accent_;
    QVector<double> series_;
    QString unit_;
    QString footer_;
    QString emptyText_ = QStringLiteral("等待数据...");
};
