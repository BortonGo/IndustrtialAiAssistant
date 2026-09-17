#include "chat_widget.hpp"
#include "message_widget.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>

ChatWidget::ChatWidget(QWidget* parent): QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(16);

    auto* btnContext = new QPushButton("Источники");
    btnContext->setCheckable(true);
    btnContext->setStyleSheet(
        "QPushButton {"
        " color: #A6ABB6;"
        " background-color: transparent;"
        " border: 1px solid #393C44;"
        " border-radius: 8px;"
        " padding: 6px 12px;"
        "}"
        "QPushButton:hover { background-color: #2C2F36; }"
        "QPushButton:checked {"
        " background-color: #293B59;"
        " color: #DCEAFF;"
        "}"
    );

    layout->addWidget(btnContext, 0, Qt::AlignRight);

    auto *hlAnswers = new QHBoxLayout;
    hlAnswers->setContentsMargins(0, 0, 0, 0);
    hlAnswers->setSpacing(16);

    messagesScroll_ = new QScrollArea;
    messagesScroll_->setWidgetResizable(true);
    messagesScroll_->setFrameShape(QFrame::NoFrame);
    messagesScroll_->setStyleSheet(
        "QScrollArea { background-color: #18191C; border: none; }"
    );
    messagesScroll_->viewport()->setObjectName("messagesViewport");
    messagesScroll_->viewport()->setStyleSheet(
        "QWidget#messagesViewport { background-color: #18191C; }"
    );

    auto* messagesContainer = new QWidget;
    messagesContainer->setObjectName("messagesContainer");
    messagesContainer->setStyleSheet(
        "QWidget#messagesContainer { background-color: #18191C; }"
    );

    messagesLayout_ = new QVBoxLayout(messagesContainer);
    messagesLayout_->setContentsMargins(8, 8, 8, 8);
    messagesLayout_->setSpacing(16);
    messagesLayout_->setAlignment(Qt::AlignTop);

    messagesScroll_->setWidget(messagesContainer);

    welcomePage_ = new QWidget;

    auto* welcomeLayout = new QVBoxLayout(welcomePage_);
    welcomeLayout->setContentsMargins(24, 24, 24, 24);
    welcomeLayout->setSpacing(12);

    auto* title = new QLabel("Чем могу помочь?");
    title->setAlignment(Qt::AlignCenter);
    title->setWordWrap(true);
    title->setStyleSheet(
        "color: #ECEEF2;"
        "font-size: 28px;"
        "font-weight: bold;"
    );

    auto* subtitle = new QLabel(
        "Задайте вопрос или загрузите документы");
    subtitle->setAlignment(Qt::AlignCenter);
    subtitle->setWordWrap(true);
    subtitle->setStyleSheet(
        "color: #A6ABB6;"
        "font-size: 16px;"
    );

    welcomeLayout->addStretch();
    welcomeLayout->addWidget(title);
    welcomeLayout->addWidget(subtitle);
    welcomeLayout->addStretch();

    conversationPages_ = new QStackedWidget;
    conversationPages_->addWidget(welcomePage_);
    conversationPages_->addWidget(messagesScroll_);

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
    textContext_->hide();

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

    textContext_->setReadOnly(true);
    hlAnswers->addWidget(conversationPages_, 7);
    hlAnswers->addWidget(textContext_, 3);

    layout->addLayout(hlAnswers, 1);

    composer_ = new MessageComposer;
    layout->addWidget(composer_);

    connect(composer_, &MessageComposer::sendRequested,
            this, &ChatWidget::sendRequested);

    connect(composer_, &MessageComposer::documentUploadRequested,
            this, &ChatWidget::documentUploadRequested);

    connect(btnContext, &QPushButton::toggled,
            textContext_, &QWidget::setVisible);

}

void ChatWidget::setMessages(const std::vector<ChatMessage>& messages) {
    while (auto* item = messagesLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    for (const auto& message : messages) {
        auto* widget = new MessageWidget(message);
        messagesLayout_->addWidget(widget);
    }

    if (messages.empty()) {
        conversationPages_->setCurrentWidget(welcomePage_);
        return;
    }

    conversationPages_->setCurrentWidget(messagesScroll_);

    QTimer::singleShot(0, this, [this]() {
        auto* scrollBar = messagesScroll_->verticalScrollBar();
        scrollBar->setValue(scrollBar->maximum());
    });
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
