#include "server_status_widget.hpp"

#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDateTime>

ServerStatusWidget::ServerStatusWidget(QWidget* parent) : QWidget(parent)
{
    setObjectName("serverStatusWidget");
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet("QWidget#serverStatusWidget { background: #24262B; border-radius: 10px; }"
                  "QPushButton { color: #B8BCC6; background: transparent; border: 1px solid #393C44;"
                  "border-radius: 6px; padding: 5px 12px; }"
                  "QPushButton:hover { background: #353C50; }");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 8, 12, 8);
    auto* row = new QHBoxLayout;
    chatLabel_ = new QLabel(this);
    chatLabel_->setObjectName("chatServerStatus");
    embeddingLabel_ = new QLabel(this);
    embeddingLabel_->setObjectName("embeddingServerStatus");
    for (auto* label : {chatLabel_, embeddingLabel_}) {
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        row->addWidget(label, 1);
    }
    auto* logButton = new QPushButton(QStringLiteral("Журнал"), this);
    logButton->setCheckable(true);
    row->addWidget(logButton);
    layout->addLayout(row);
    storageLabel_ = new QLabel(this);
    storageLabel_->setTextFormat(Qt::PlainText);
    storageLabel_->setWordWrap(true);
    layout->addWidget(storageLabel_);
    setStorageStatus(QStringLiteral("Ожидание запуска…"));
    auto* documentRow = new QHBoxLayout;
    documentLabel_ = new QLabel(this);
    documentLabel_->setObjectName("documentProgress");
    documentLabel_->setTextFormat(Qt::PlainText);
    documentLabel_->setWordWrap(true);
    documentLabel_->setStyleSheet("color: #B8BCC6; font-size: 13px;");
    cancelButton_ = new QPushButton(QStringLiteral("Отмена"), this);
    cancelButton_->setObjectName("cancelDocument");
    documentRow->addWidget(documentLabel_, 1);
    documentRow->addWidget(cancelButton_);
    layout->addLayout(documentRow);
    documentLabel_->hide();
    cancelButton_->hide();
    connect(cancelButton_, &QPushButton::clicked, this, &ServerStatusWidget::cancelDocumentRequested);
    log_ = new QPlainTextEdit(this);
    log_->setObjectName("serverLog");
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(500);
    log_->setFixedHeight(140);
    log_->setStyleSheet("QPlainTextEdit { background: #18191C; color: #B8BCC6;"
                       "border: 1px solid #393C44; font-family: Consolas; font-size: 12px; }");
    log_->hide();
    layout->addWidget(log_);
    connect(logButton, &QPushButton::toggled, log_, &QWidget::setVisible);
    setChatStatus(QStringLiteral("Остановлен"));
    setEmbeddingStatus(QStringLiteral("Остановлен"));
}

void ServerStatusWidget::setStorageStatus(const QString& text) { setStatus(storageLabel_, QStringLiteral("БД"), text); }

void ServerStatusWidget::setStatus(QLabel* label, const QString& name, const QString& text)
{
    const QString color = text.contains(QStringLiteral("Ошибка")) ? QStringLiteral("#F19B9B")
                         : text.contains(QStringLiteral("Готов")) ? QStringLiteral("#9FD7AF")
                                                                  : QStringLiteral("#DCC28A");
    label->setStyleSheet(QStringLiteral("color: %1; font-size: 13px;").arg(color));
    label->setText(name + QStringLiteral(": ") + text);
    label->setToolTip(label->text());
}

void ServerStatusWidget::setChatStatus(const QString& text)
{
    setStatus(chatLabel_, QStringLiteral("Qwen"), text);
}

void ServerStatusWidget::setEmbeddingStatus(const QString& text)
{
    setStatus(embeddingLabel_, QStringLiteral("EmbeddingGemma"), text);
}

void ServerStatusWidget::appendLog(const QString& server, const QString& text)
{
    log_->appendPlainText(QStringLiteral("[%1] [%2] %3")
                             .arg(QDateTime::currentDateTime().toString("HH:mm:ss"), server, text));
}

void ServerStatusWidget::setDocumentProgress(const QString& text, bool canCancel)
{
    documentLabel_->setText(text);
    documentLabel_->setVisible(!text.isEmpty());
    cancelButton_->setVisible(canCancel);
}
