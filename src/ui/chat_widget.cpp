#include "chat_widget.hpp"
#include "message_widget.hpp"
#include "thinking_indicator.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>

ChatWidget::ChatWidget(QWidget* parent): QWidget(parent) {
    auto* outerLayout = new QHBoxLayout(this);
    outerLayout->setContentsMargins(16, 8, 16, 12);
    outerLayout->setSpacing(0);
    auto* column = new QWidget;
    column->setObjectName("conversationColumn");
    column->setMaximumWidth(1040);
    column->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    outerLayout->addStretch();
    outerLayout->addWidget(column, 1);
    outerLayout->addStretch();

    auto* layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

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

    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(12);
    layout->addLayout(header);

    auto *hlAnswers = new QHBoxLayout;
    hlAnswers->setContentsMargins(0, 0, 0, 0);
    hlAnswers->setSpacing(16);

    messagesScroll_ = new QScrollArea;
    messagesScroll_->setWidgetResizable(true);
    messagesScroll_->setFrameShape(QFrame::NoFrame);
    auto* scrollBar = messagesScroll_->verticalScrollBar();
    connect(scrollBar, &QScrollBar::valueChanged, this, [this, scrollBar](int value) {
        if (!rebuilding_) followTail_ = scrollBar->maximum() - value <= 24;
    });
    connect(scrollBar, &QScrollBar::rangeChanged, this, [this, scrollBar](int, int maximum) {
        if (!rebuilding_ && followTail_) scrollBar->setValue(maximum);
    });
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
    messagesLayout_->setContentsMargins(0, 8, 8, 8);
    messagesLayout_->setSpacing(16);
    messagesLayout_->setAlignment(Qt::AlignTop);
    messagesLayout_->setSizeConstraint(QLayout::SetMinimumSize);

    thinkingIndicator_ = new ThinkingIndicator(messagesContainer);
    messagesLayout_->addWidget(thinkingIndicator_, 0, Qt::AlignLeft);
    thinkingIndicator_->hide();

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
        "Задайте вопрос или загрузите документ или изображение");
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
    notificationLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    notificationLabel_->setStyleSheet(
        "QLabel {"
        " color: #B8BCC6;"
        " font-size: 14px;"
        " padding: 6px;"
        "}"
    );
    notificationLabel_->hide();

    header->addWidget(notificationLabel_, 1);
    header->addStretch();
    header->addWidget(btnContext, 0, Qt::AlignTop);

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

void ChatWidget::setMessages(const std::vector<ChatMessage>& messages, bool scrollToBottom) {
    const auto version = ++viewVersion_;
    const int position = messagesScroll_->verticalScrollBar()->value();
    if (scrollToBottom) followTail_ = true;
    rebuilding_ = true;
    streamingMessage_ = nullptr;
    thinkingIndicator_->setWriting(false);
    while (auto* item = messagesLayout_->takeAt(0)) {
        if (item->widget() != thinkingIndicator_) delete item->widget();
        delete item;
    }

    for (const auto& message : messages) {
        auto* widget = new MessageWidget(message);
        messagesLayout_->addWidget(widget);
        widget->show();
    }

    messagesLayout_->addWidget(thinkingIndicator_, 0, Qt::AlignLeft);
    hasMessages_ = !messages.empty();
    if (!hasMessages_ && !waiting_) {
        conversationPages_->setCurrentWidget(welcomePage_);
        rebuilding_ = false;
        return;
    }

    conversationPages_->setCurrentWidget(messagesScroll_);
    messagesLayout_->activate();

    QTimer::singleShot(0, this, [this, version, position]() {
        if (version != viewVersion_) return;
        auto* scrollBar = messagesScroll_->verticalScrollBar();
        scrollBar->setValue(followTail_ ? scrollBar->maximum() : position);
        rebuilding_ = false;
    });
}

void ChatWidget::setStreamingAnswer(const QString& text) {
    if (text.isEmpty()) {
        delete streamingMessage_; streamingMessage_ = nullptr;
    } else {
        ChatMessage message; message.role = MessageRole::Assistant; message.text = text; message.status = "streaming";
        if (!streamingMessage_) {
            streamingMessage_ = new MessageWidget(message);
            streamingMessage_->setObjectName("streamingAnswer");
            messagesLayout_->insertWidget(messagesLayout_->indexOf(thinkingIndicator_), streamingMessage_);
        } else streamingMessage_->setMessage(message);
    }
    thinkingIndicator_->setWriting(streamingMessage_ != nullptr);
    conversationPages_->setCurrentWidget(hasMessages_ || waiting_ || streamingMessage_ ? messagesScroll_ : welcomePage_);
}

void ChatWidget::setWaitingForAnswer(bool waiting) {
    if (waiting_ == waiting) return;
    waiting_ = waiting;
    if (waiting) followTail_ = true;
    thinkingIndicator_->setVisible(waiting);
    conversationPages_->setCurrentWidget(hasMessages_ || waiting || streamingMessage_ ? messagesScroll_ : welcomePage_);
}

void ChatWidget::setContextText(const QString& text) {
    textContext_->setPlainText(text);
}

void ChatWidget::showNotification(const QString& text) {
    notificationLabel_->setText(text);
    notificationLabel_->setVisible(!text.isEmpty());
}

void ChatWidget::clearInput() {
    composer_->clearInput();
}

void ChatWidget::setSendEnabled(bool enabled) { composer_->setSendEnabled(enabled); }
QString ChatWidget::inputText() const { return composer_->inputText(); }
void ChatWidget::setInputText(const QString& text) { composer_->setInputText(text); }
void ChatWidget::setUploadEnabled(bool enabled) { composer_->setUploadEnabled(enabled); }
QStringList ChatWidget::attachmentPaths() const { return composer_->attachmentPaths(); }
void ChatWidget::setAttachmentPaths(const QStringList& paths) { composer_->setAttachmentPaths(paths); }
