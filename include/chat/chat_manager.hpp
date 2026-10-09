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
    QString pendingMessageId_;
    QString pendingAnswer_;
    QString deletingChatId_;
    bool operation_ = true;
    bool finishing_ = false;
    AssistantService* service_ = nullptr;
public:
    explicit ChatManager(AssistantService* service, QObject* parent = nullptr);

    QString createChat();
    bool selectChat(const QString& chatId);
    bool deleteChat(const QString& chatId);
    bool canDeleteChat(const QString& chatId) const;
    bool isAwaitingAnswer(const QString& chatId) const { return !chatId.isEmpty() && chatId == pendingChatId_; }
    QString pendingAnswer(const QString& chatId) const { return isAwaitingAnswer(chatId) ? pendingAnswer_ : QString(); }

    int chatCount() const;
    const Chat* chatAt(int row) const;

    bool sendMessage(const QString& text, const QStringList& attachments = {});
    const Chat* currentChat() const;
    bool isBusy() const { return operation_ || !pendingChatId_.isEmpty(); }
    QString chatActivity(const QString& chatId) const;

signals:
    void chatActivityChanged();
    void chatsAboutToReset();
    void chatsReset();
    void operationChanged();
    void chatCreated(const QString& chatId, const QString& title);
    void currentChatChanged(const QString& chatId);

    void chatAboutToBeCreated(int row);
    void chatAboutToBeRemoved(int row);
    void chatRemoved(const QString& chatId);
    void notification(const QString& message);

    void messageAdded(const QString& chatId, const QString& messageId);
    void answerProgress(const QString& chatId, const QString& text);

    void errorOccurred(const QString& message);

    void chatTitleChanged(const QString& chatId);

private:
    const Chat* findChat(const QString& chatId) const;
    Chat* findChat(const QString& chatId);
    QString appendMessage(const QString& chatId, MessageRole role, const QString& text,
                          const QString& id, const QString& status = "complete", const QString& error = {}, const QString& replyTo = {});
    void restore();
    void finish(const QString& answer, const QString& error);
};

