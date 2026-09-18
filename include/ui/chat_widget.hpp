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

class ChatWidget : public QWidget {
    Q_OBJECT

    QScrollArea* messagesScroll_;
    QVBoxLayout* messagesLayout_;
    QTextEdit* textContext_;
    QLabel* notificationLabel_;
    MessageComposer* composer_;
    QStackedWidget* conversationPages_;
    QWidget* welcomePage_;
public:
    explicit ChatWidget(QWidget* parent = nullptr);

    void setMessages(const std::vector<ChatMessage>& messages);
    void setContextText(const QString& text);
    void showNotification(const QString& text);
    void clearInput();

signals:
    void sendRequested(const QString& text);
    void documentUploadRequested();
};
