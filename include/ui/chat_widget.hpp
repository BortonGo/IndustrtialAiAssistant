#pragma once
#include "chat/chat.hpp"
#include "message_composer.hpp"

#include <QWidget>
#include <QString>
#include <QTextEdit>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QStackedWidget>

#include <vector>

class ThinkingIndicator;
class MessageWidget;

class ChatWidget : public QWidget {
    Q_OBJECT

    QScrollArea* messagesScroll_;
    QVBoxLayout* messagesLayout_;
    QTextEdit* textContext_;
    QLabel* notificationLabel_;
    QLabel* chatTitle_;
    QWidget* sourcesPanel_;
    MessageComposer* composer_;
    QStackedWidget* conversationPages_;
    QWidget* welcomePage_;
    ThinkingIndicator* thinkingIndicator_;
    bool waiting_ = false;
    bool hasMessages_ = false;
    MessageWidget* streamingMessage_ = nullptr;
    bool followTail_ = true;
    bool rebuilding_ = false;
    quint64 viewVersion_ = 0;
public:
    explicit ChatWidget(QWidget* parent = nullptr);

    void setMessages(const std::vector<ChatMessage>& messages, bool scrollToBottom = true);
    void setStreamingAnswer(const QString& text);
    void setWaitingForAnswer(bool waiting);
    void setContextText(const QString& text);
    void setChatTitle(const QString& title);
    void showNotification(const QString& text);
    void clearInput();
    QString inputText() const;
    void setInputText(const QString& text);
    void setSendEnabled(bool enabled);
    void setUploadEnabled(bool enabled);
    QStringList attachmentPaths() const;
    void setAttachmentPaths(const QStringList& paths);

signals:
    void sendRequested(const QString& text);
    void documentUploadRequested();
    void sidebarToggleRequested();
    void documentsPageRequested();
};
