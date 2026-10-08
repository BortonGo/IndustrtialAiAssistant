#include "thinking_indicator.hpp"
#include <QTimer>
#include <QPainter>
#include <QPen>

ThinkingIndicator::ThinkingIndicator(QWidget* parent) : QWidget(parent), timer_(new QTimer(this)) {
    setObjectName("thinkingIndicator");
    setFixedSize(172, 42);
    setAccessibleName(QStringLiteral("Готовлю ответ"));
    timer_->setInterval(32);
    connect(timer_, &QTimer::timeout, this, [this]() { update(); });
}

void ThinkingIndicator::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    elapsed_.start(); timer_->start();
}

void ThinkingIndicator::hideEvent(QHideEvent* event) {
    timer_->stop(); QWidget::hideEvent(event);
}

void ThinkingIndicator::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor("#242527"));
    painter.drawRoundedRect(rect(), 12, 12);
    const QRectF ring(14, 12, 18, 18);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor("#454649"), 2.5));
    painter.drawEllipse(ring);
    painter.setPen(QPen(QColor("#CCD7E8"), 2.5, Qt::SolidLine, Qt::RoundCap));
    const int angle = elapsed_.isValid() ? static_cast<int>(elapsed_.elapsed() % 1200) * 360 / 1200 : 0;
    painter.drawArc(ring, (90 - angle) * 16, -110 * 16);
    QFont textFont("Segoe UI"); textFont.setPixelSize(14);
    painter.setFont(textFont); painter.setPen(QColor("#ECECEE"));
    painter.drawText(QRect(44, 0, width() - 56, height()), Qt::AlignVCenter,
                     writing_ ? QStringLiteral("Пишу ответ") : QStringLiteral("Готовлю ответ"));
}
