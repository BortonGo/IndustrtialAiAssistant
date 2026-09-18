#pragma once

#include "chat.hpp"
#include "assistant_service.hpp"

#include <QObject>
#include <QString>

#include <vector>

class ChatManager final : public QObject {
    Q_OBJECT

    std::vector<Chat> chats_;
    QString currentChatId_;
    QString pendingChatId_;
    AssistantService* service_ = nullptr;
public:
    explicit ChatManager(AssistantService* service, QObject* parent = nullptr);

    QString createChat();
    bool selectChat(const QString& chatId);

    int chatCount() const;
    const Chat* chatAt(int row) const;

    bool sendMessage(const QString& text);
    const Chat* currentChat() const;

signals:
    void chatCreated(const QString& chatId, const QString& title);
    void currentChatChanged(const QString& chatId);

    void chatAboutToBeCreated(int row);

    void messageAdded(const QString& chatId, const QString& messageId);

    void errorOccurred(const QString& message);

    void chatTitleChanged(const QString& chatId);

private:
    const Chat* findChat(const QString& chatId) const;
    Chat* findChat(const QString& chatId);
    QString appendMessage(const QString& chatId, MessageRole role, const QString& text);
};

