#include "chat_widget.hpp"
#include "source_panel.hpp"
#include "message_widget.hpp"
#include "thinking_indicator.hpp"
#include "ui_style.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QToolButton>

ChatWidget::ChatWidget(QWidget* parent): QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0); mainLayout->setSpacing(0);
    auto* toolbar = new QWidget;
    toolbar->setObjectName("workspaceHeader"); toolbar->setAttribute(Qt::WA_StyledBackground, true);
    toolbar->setFixedHeight(58);
    auto* header = new QHBoxLayout(toolbar);
    header->setContentsMargins(18, 10, 18, 10); header->setSpacing(12);
    auto* sidebarButton = new QToolButton;
    sidebarButton->setObjectName("toggleSidebar");
    sidebarButton->setIcon(UiStyle::icon(UiStyle::Icon::Sidebar)); sidebarButton->setFixedSize(32, 32);
    sidebarButton->setToolTip(QStringLiteral("Показать или скрыть чаты"));
    sidebarButton->setAccessibleName(sidebarButton->toolTip());
    header->addWidget(sidebarButton);
    chatTitle_ = new QLabel(QStringLiteral("Новый чат"));
    chatTitle_->setObjectName("chatTitle"); chatTitle_->setTextFormat(Qt::PlainText);
    chatTitle_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    chatTitle_->setStyleSheet("color: #ECECEE; font-size: 14px; font-weight: 600;");
    header->addWidget(chatTitle_, 1);
    mainLayout->addWidget(toolbar);
    connect(sidebarButton, &QToolButton::clicked, this, &ChatWidget::sidebarToggleRequested);

    auto* body = new QWidget;
    auto* bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0); bodyLayout->setSpacing(0);
    auto* conversationHolder = new QWidget;
    auto* outerLayout = new QHBoxLayout(conversationHolder);
    outerLayout->setContentsMargins(28, 0, 28, 16);
    outerLayout->setSpacing(0);
    auto* column = new QWidget;
    column->setObjectName("conversationColumn");
    column->setMaximumWidth(900);
    column->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    outerLayout->addStretch();
    outerLayout->addWidget(column, 1);
    outerLayout->addStretch();

    auto* layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    auto* btnContext = new QPushButton("Источники");
    btnContext->setCheckable(true);
    btnContext->setIcon(UiStyle::icon(UiStyle::Icon::Sources));
    btnContext->setObjectName("toggleSources");
    btnContext->setStyleSheet(
        "QPushButton {"
        " color: #A5A6AA;"
        " background-color: transparent;"
        " border: none;"
        " border-radius: 7px;"
        " padding: 6px 12px;"
        "}"
        "QPushButton:hover { background-color: #2D2E30; }"
        "QPushButton:checked {"
        " background-color: #323437;"
        " color: #ECECEE;"
        "}"
    );

    header->addWidget(btnContext);

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
        "QScrollArea { background-color: #191A1B; border: none; }"
    );
    messagesScroll_->viewport()->setObjectName("messagesViewport");
    messagesScroll_->viewport()->setStyleSheet(
        "QWidget#messagesViewport { background-color: #191A1B; }"
    );

    auto* messagesContainer = new QWidget;
    messagesContainer->setObjectName("messagesContainer");
    messagesContainer->setStyleSheet(
        "QWidget#messagesContainer { background-color: #191A1B; }"
    );

    messagesLayout_ = new QVBoxLayout(messagesContainer);
    messagesLayout_->setContentsMargins(4, 26, 12, 12);
    messagesLayout_->setSpacing(24);
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
        "color: #ECECEE;"
        "font-size: 26px;"
        "font-weight: 500;"
    );

    auto* subtitle = new QLabel(
        "Задайте вопрос или загрузите документ или изображение");
    subtitle->setAlignment(Qt::AlignCenter);
    subtitle->setWordWrap(true);
    subtitle->setStyleSheet(
        "color: #A5A6AA;"
        "font-size: 14px;"
    );

    welcomeLayout->addStretch();
    welcomeLayout->addWidget(title);
    welcomeLayout->addWidget(subtitle);
    welcomeLayout->addStretch();

    conversationPages_ = new QStackedWidget;
    conversationPages_->addWidget(welcomePage_);
    conversationPages_->addWidget(messagesScroll_);

    sourcesPanel_ = new QWidget;
    sourcesPanel_->setObjectName("sourcesPanel");
    sourcesPanel_->setAttribute(Qt::WA_StyledBackground, true);
    sourcesPanel_->setFixedWidth(300);
    sourcesPanel_->setStyleSheet("QWidget#sourcesPanel { background: #202122; border-left: 1px solid #363739; }");
    auto* sourcesLayout = new QVBoxLayout(sourcesPanel_);
    sourcesLayout->setContentsMargins(16, 14, 12, 14); sourcesLayout->setSpacing(12);
    auto* sourcesHeading = new QHBoxLayout;
    sourcesHeading->addWidget(new QLabel(QStringLiteral("Источники ответа")), 1);
    auto* closeSources = new QToolButton;
    closeSources->setIcon(UiStyle::icon(UiStyle::Icon::Close));
    closeSources->setAccessibleName(QStringLiteral("Закрыть источники"));
    sourcesHeading->addWidget(closeSources); sourcesLayout->addLayout(sourcesHeading);
    connect(closeSources, &QToolButton::clicked, btnContext, [btnContext]() { btnContext->setChecked(false); });
    auto* documents = new QPushButton(QStringLiteral("Документы чата"));
    documents->setIcon(UiStyle::icon(UiStyle::Icon::Documents));
    sourcesLayout->addWidget(documents, 0, Qt::AlignLeft);
    connect(documents, &QPushButton::clicked, this, &ChatWidget::documentsPageRequested);
    sourceView_ = new SourcePanel;
    sourcesToggle_ = btnContext;
    sourcesLayout->addWidget(sourceView_, 1);
    sourcesPanel_->hide();

    notificationLabel_ = new QLabel;
    notificationLabel_->setTextFormat(Qt::PlainText);
    notificationLabel_->setWordWrap(true);
    notificationLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    notificationLabel_->setStyleSheet(
        "QLabel {"
        " color: #A5A6AA;"
        " font-size: 12px;"
        " padding: 8px 28px;"
        "}"
    );
    notificationLabel_->hide();

    mainLayout->addWidget(notificationLabel_);
    layout->addWidget(conversationPages_, 1);

    composer_ = new MessageComposer;
    layout->addWidget(composer_);
    auto* privacy = new QLabel(QStringLiteral("Файлы и переписка остаются на компьютере"));
    privacy->setAlignment(Qt::AlignCenter);
    privacy->setStyleSheet("color: #84858A; font-size: 11px;");
    layout->addWidget(privacy);
    bodyLayout->addWidget(conversationHolder, 1);
    bodyLayout->addWidget(sourcesPanel_);
    mainLayout->addWidget(body, 1);

    connect(composer_, &MessageComposer::sendRequested,
            this, &ChatWidget::sendRequested);

    connect(composer_, &MessageComposer::documentUploadRequested,
            this, &ChatWidget::documentUploadRequested);
    connect(composer_, &MessageComposer::attachmentError,
            this, &ChatWidget::showNotification);

    connect(btnContext, &QPushButton::toggled,
            sourcesPanel_, &QWidget::setVisible);

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

    sourceQuestions_.clear();
    QString latest;
    for (const auto& message : messages) if (message.role == MessageRole::User) {
        sourceQuestions_.insert(message.id, message); latest = message.id;
    }
    if (latest != latestQuestion_ || !sourceQuestions_.contains(selectedQuestion_)) selectedQuestion_ = latest;
    latestQuestion_ = latest;
    showSources(selectedQuestion_, false);
    for (const auto& message : messages) {
        auto* widget = new MessageWidget(message);
        const auto question = sourceQuestions_.value(message.replyTo);
        widget->setSourcesAvailable(!question.contextSnapshot.isEmpty() || !question.sourceReferences.isEmpty(), question.sourceReferences.size());
        const auto replyTo = message.replyTo;
        connect(widget, &MessageWidget::sourcesRequested, this, [this, replyTo]() { showSources(replyTo, true); });
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

void ChatWidget::showSources(const QString& questionId, bool openPanel) {
    selectedQuestion_ = questionId;
    const auto question = sourceQuestions_.value(questionId);
    sourceView_->setSources(question.text, question.contextSnapshot, question.sourceReferences);
    if (openPanel) sourcesToggle_->setChecked(true);
}

void ChatWidget::setChatTitle(const QString& title) {
    chatTitle_->setText(title.isEmpty() ? QStringLiteral("Новый чат") : title);
    chatTitle_->setToolTip(chatTitle_->text());
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
void ChatWidget::addAttachmentPaths(const QStringList& paths) { composer_->addAttachmentPaths(paths); }
