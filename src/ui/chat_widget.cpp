#include "chat_widget.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>

ChatWidget::ChatWidget(QWidget* parent)
    : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(16);
    auto *hlAnswers = new QHBoxLayout;
    hlAnswers->setContentsMargins(0, 0, 0, 0);
    hlAnswers->setSpacing(16);

    textAnswer_ = new QTextEdit;
    textAnswer_->setStyleSheet(
        "QTextEdit {"
        " background-color: #24262B;"
        " color: #E8E8ED;"
        " border: 1px solid #393C44;"
        " border-radius: 14px;"
        " padding: 12px;"
        " font-size: 18px;"
        "}"
    );

    textContext_ = new QTextEdit;
    textContext_->setStyleSheet(
        "QTextEdit {"
        " background-color: #24262B;"
        " color: #E8E8ED;"
        " border: 1px solid #393C44;"
        " border-radius: 14px;"
        " padding: 12px;"
        " font-size: 18px;"
        "}"
    );

    notificationLabel_ = new QLabel;
    notificationLabel_->setTextFormat(Qt::PlainText);
    notificationLabel_->setWordWrap(true);
    notificationLabel_->setStyleSheet(
        "QLabel {"
        " color: #B8BCC6;"
        " font-size: 14px;"
        " padding: 6px;"
        "}"
    );
    notificationLabel_->hide();

    layout->insertWidget(0, notificationLabel_);

    textAnswer_->setReadOnly(true);
    textContext_->setReadOnly(true);
    hlAnswers->addWidget(textAnswer_, 7);
    hlAnswers->addWidget(textContext_, 3);

    layout->addLayout(hlAnswers, 1);

    composer_ = new MessageComposer;
    layout->addWidget(composer_);

    connect(composer_, &MessageComposer::sendRequested,
            this, &ChatWidget::sendRequested);

    connect(composer_, &MessageComposer::documentUploadRequested,
            this, &ChatWidget::documentUploadRequested);
}

void ChatWidget::setMessages(const std::vector<ChatMessage>& messages) {
    QString ans;
    for (const auto& m : messages) {
        if (m.role == MessageRole::User) {
            ans += "You: \n\n" + m.text + "\n\n";
        } else {
            ans += "Assistant: \n\n" + m.text + "\n\n";
        }
    }
    textAnswer_->setPlainText(ans);
}

void ChatWidget::setContextText(const QString& text) {
    textContext_->setPlainText(text);
}

void ChatWidget::showNotification(const QString& text) {
    notificationLabel_->setText(text);
    notificationLabel_->show();
}

void ChatWidget::clearInput() {
    composer_->clearInput();
}
