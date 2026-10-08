#include "server_status_widget.hpp"
#include "ui_style.hpp"

#include <QApplication>
#include <QDesktopWidget>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDateTime>
#include <QHideEvent>

ServerStatusWidget::ServerStatusWidget(QWidget* parent) : QWidget(parent) {
    setObjectName("serverStatusWidget");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    summaryButton_ = new QPushButton(this);
    summaryButton_->setObjectName("serverSummary");
    summaryButton_->setMinimumHeight(36);
    layout->addWidget(summaryButton_);

    details_ = new QFrame(this, Qt::Popup);
    details_->setAttribute(Qt::WA_NoMouseReplay);
    details_->setObjectName("serverDetails");
    details_->setFixedWidth(400);
    details_->setStyleSheet(UiStyle::applicationStyleSheet() +
        QStringLiteral("QFrame#serverDetails { background: #242527; border: 1px solid #363739; border-radius: 10px; }"));
    auto* detailsLayout = new QVBoxLayout(details_);
    detailsLayout->setContentsMargins(18, 12, 18, 16);
    detailsLayout->setSpacing(10);
    auto* heading = new QHBoxLayout;
    auto* title = new QLabel(QStringLiteral("Локальные сервисы"));
    title->setStyleSheet("font-size: 15px; font-weight: 600;");
    auto* close = new QToolButton;
    close->setIcon(UiStyle::icon(UiStyle::Icon::Close));
    close->setAccessibleName(QStringLiteral("Закрыть состояние серверов"));
    heading->addWidget(title, 1); heading->addWidget(close);
    detailsLayout->addLayout(heading);
    connect(close, &QToolButton::clicked, details_, &QWidget::hide);

    chatLabel_ = new QLabel; chatLabel_->setObjectName("chatServerStatus");
    embeddingLabel_ = new QLabel; embeddingLabel_->setObjectName("embeddingServerStatus");
    storageLabel_ = new QLabel; storageLabel_->setObjectName("storageStatus");
    for (auto* label : {chatLabel_, embeddingLabel_, storageLabel_}) {
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        detailsLayout->addWidget(label);
    }
    usageLabel_ = new QLabel;
    usageLabel_->setStyleSheet("color: #A5A6AA; font-size: 12px; padding-top: 8px;");
    usageLabel_->hide(); detailsLayout->addWidget(usageLabel_);
    auto* logButton = new QPushButton(QStringLiteral("Журнал запуска"));
    logButton->setCheckable(true); logButton->setObjectName("toggleServerLog");
    detailsLayout->addWidget(logButton, 0, Qt::AlignLeft);
    log_ = new QPlainTextEdit;
    log_->setObjectName("serverLog"); log_->setReadOnly(true);
    log_->setMaximumBlockCount(500); log_->setFixedHeight(170);
    log_->setStyleSheet("QPlainTextEdit { background: #191A1B; color: #A5A6AA; border: 1px solid #363739;"
                       "border-radius: 6px; font-family: Consolas; font-size: 12px; padding: 6px; }");
    log_->hide(); detailsLayout->addWidget(log_);
    connect(logButton, &QPushButton::toggled, this, [this](bool visible) {
        log_->setVisible(visible); positionDetails();
    });
    connect(summaryButton_, &QPushButton::clicked, this, [this]() {
        if (details_->isVisible()) details_->hide();
        else { positionDetails(); details_->show(); }
    });
    setChatStatus(QStringLiteral("Остановлен"));
    setEmbeddingStatus(QStringLiteral("Остановлен"));
    setStorageStatus(QStringLiteral("Ожидание запуска…"));
}

void ServerStatusWidget::hideEvent(QHideEvent* event) {
    details_->hide(); QWidget::hideEvent(event);
}

void ServerStatusWidget::positionDetails() {
    details_->adjustSize();
    const QPoint anchor = mapToGlobal(QPoint(0, 0));
    const QRect available = QApplication::desktop()->availableGeometry(this);
    const int x = qBound(available.left(), anchor.x(), qMax(available.left(), available.right() - details_->width() + 1));
    const int y = qBound(available.top(), anchor.y() - details_->height() - 8,
                         qMax(available.top(), available.bottom() - details_->height() + 1));
    details_->move(x, y);
}

void ServerStatusWidget::updateSummary() {
    bool ready = true, failed = false;
    QStringList statuses;
    for (auto* label : {chatLabel_, embeddingLabel_, storageLabel_}) {
        const QString text = label->property("statusText").toString();
        ready = ready && text.startsWith(QStringLiteral("Готов"), Qt::CaseInsensitive);
        failed = failed || text.contains(QStringLiteral("Ошибка"), Qt::CaseInsensitive);
        statuses.append(label->text());
    }
    summaryButton_->setText(failed ? QStringLiteral("Ошибка сервисов") :
        ready ? QStringLiteral("Модели готовы") : QStringLiteral("Сервисы не готовы"));
    const QString color = failed ? "#F19B9B" : ready ? "#8EC7A6" : "#CDBA94";
    QPixmap dot(12, 12); dot.fill(Qt::transparent);
    { QPainter painter(&dot); painter.setRenderHint(QPainter::Antialiasing);
      painter.setPen(Qt::NoPen); painter.setBrush(QColor(color)); painter.drawEllipse(QRectF(3, 3, 6, 6)); }
    summaryButton_->setIcon(QIcon(dot)); summaryButton_->setIconSize(QSize(12, 12));
    summaryButton_->setStyleSheet(QStringLiteral("QPushButton { color: %1; font-size: 12px; text-align: left;"
        "padding: 8px; border: none; border-radius: 7px; } QPushButton:hover { background: #2D2E30; }").arg(failed ? color : "#A5A6AA"));
    summaryButton_->setToolTip(statuses.join("\n") + QStringLiteral("\nНажмите, чтобы открыть состояние и журнал"));
    if (details_->isVisible()) positionDetails();
}

void ServerStatusWidget::setStatus(QLabel* label, const QString& name, const QString& text) {
    const QString color = text.contains(QStringLiteral("Ошибка"), Qt::CaseInsensitive) ? "#F19B9B" :
        text.startsWith(QStringLiteral("Готов"), Qt::CaseInsensitive) ? "#8EC7A6" : "#CDBA94";
    label->setProperty("statusText", text);
    label->setStyleSheet(QStringLiteral("color: %1; font-size: 13px;").arg(color));
    label->setText(name + QStringLiteral(": ") + text); label->setToolTip(label->text());
    updateSummary();
}

void ServerStatusWidget::setChatStatus(const QString& text) { setStatus(chatLabel_, "Qwen", text); }
void ServerStatusWidget::setEmbeddingStatus(const QString& text) { setStatus(embeddingLabel_, "EmbeddingGemma", text); }
void ServerStatusWidget::setStorageStatus(const QString& text) { setStatus(storageLabel_, QStringLiteral("БД"), text); }
void ServerStatusWidget::setSystemUsage(double cpuPercent, double memoryPercent) {
    usageLabel_->setText(QStringLiteral("CPU: %1%   ·   RAM: %2%").arg(cpuPercent, 0, 'f', 1).arg(memoryPercent, 0, 'f', 1));
    usageLabel_->show();
}
void ServerStatusWidget::appendLog(const QString& server, const QString& text) {
    log_->appendPlainText(QStringLiteral("[%1] [%2] %3").arg(QDateTime::currentDateTime().toString("HH:mm:ss"), server, text));
}
