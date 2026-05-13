#include "trend_chart_widget.h"

#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>

TrendChartWidget::TrendChartWidget(QWidget* parent)
    : QWidget(parent), accent_(QColor(QStringLiteral("#57b5ff"))) {
    setMinimumHeight(180);
}

TrendChartWidget::TrendChartWidget(const QString& title, const QColor& accent, QWidget* parent)
    : QWidget(parent), title_(title), accent_(accent) {
    setMinimumHeight(180);
}

void TrendChartWidget::setSeries(const QVector<double>& series, const QString& unit, const QString& footer) {
    series_ = series;
    unit_ = unit;
    footer_ = footer;
    update();
}

void TrendChartWidget::setEmptyText(const QString& text) {
    emptyText_ = text;
    update();
}

void TrendChartWidget::setPresentation(const QString& title, const QColor& accent) {
    title_ = title;
    accent_ = accent;
    update();
}

void TrendChartWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF bounds = rect().adjusted(1, 1, -1, -1);
    painter.setPen(QColor(QStringLiteral("#24324f")));
    painter.setBrush(QColor(QStringLiteral("#121a2b")));
    painter.drawRoundedRect(bounds, 18, 18);

    painter.setPen(QColor(QStringLiteral("#f3f7ff")));
    QFont titleFont = font();
    titleFont.setPointSize(titleFont.pointSize() + 1);
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.drawText(QRectF(18, 12, width() - 36, 24), Qt::AlignLeft | Qt::AlignVCenter, title_);

    if (series_.isEmpty()) {
        painter.setPen(QColor(QStringLiteral("#8ea3c4")));
        painter.setFont(font());
        painter.drawText(QRectF(18, 52, width() - 36, height() - 70),
                         Qt::AlignCenter,
                         emptyText_);
        return;
    }

    const double minValue = *std::min_element(series_.begin(), series_.end());
    const double maxValue = *std::max_element(series_.begin(), series_.end());
    const double range = qMax(1.0, maxValue - minValue);

    const QRectF plot = QRectF(18, 52, width() - 36, height() - 86);
    painter.setPen(QPen(QColor(QStringLiteral("#1f2b45")), 1));
    for (int i = 0; i < 4; ++i) {
        const double y = plot.top() + plot.height() * i / 3.0;
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }

    QPainterPath linePath;
    QPainterPath fillPath;
    for (int i = 0; i < series_.size(); ++i) {
        const double x = plot.left() + plot.width() * i / qMax(1, series_.size() - 1);
        const double normalized = (series_[i] - minValue) / range;
        const double y = plot.bottom() - normalized * plot.height();
        const QPointF point(x, y);

        if (i == 0) {
            linePath.moveTo(point);
            fillPath.moveTo(QPointF(point.x(), plot.bottom()));
            fillPath.lineTo(point);
        } else {
            linePath.lineTo(point);
            fillPath.lineTo(point);
        }
    }

    fillPath.lineTo(QPointF(plot.right(), plot.bottom()));
    fillPath.closeSubpath();

    QColor fillColor = accent_;
    fillColor.setAlpha(55);
    painter.fillPath(fillPath, fillColor);
    painter.setPen(QPen(accent_, 2.5));
    painter.drawPath(linePath);

    painter.setPen(QColor(QStringLiteral("#e7eefb")));
    painter.setFont(font());
    painter.drawText(QRectF(plot.left(), plot.bottom() + 8, plot.width() / 2.0, 20),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("Min %1 %2").arg(QString::number(minValue, 'f', 1), unit_));
    painter.drawText(QRectF(plot.left() + plot.width() / 2.0, plot.bottom() + 8, plot.width() / 2.0, 20),
                     Qt::AlignRight | Qt::AlignVCenter,
                     footer_.isEmpty()
                         ? QStringLiteral("Max %1 %2").arg(QString::number(maxValue, 'f', 1), unit_)
                         : footer_);
}
