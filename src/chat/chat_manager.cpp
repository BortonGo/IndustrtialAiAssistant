#include "chat_manager.hpp"

#include <QUuid>
#include <QDateTime>
#include <QString>

#include <utility>

ChatManager::ChatManager(AssistantService* service, QObject* parent)
    : QObject(parent), service_(service) {
    connect(service_, &AssistantService::answerReady,
            this, [this](const QString& answer) {
        if (pendingChatId_.isEmpty()) {
            return;
        }
        const QString targetChatId = pendingChatId_;
        pendingChatId_.clear();
        appendMessage(targetChatId, MessageRole::Assistant, answer);
    });

    connect(service_, &AssistantService::questionFailed,
            this, [this](const QString& message) {
        if (pendingChatId_.isEmpty()) {
            return;
        }
        pendingChatId_.clear();
        emit errorOccurred(message);
    });
}

QString ChatManager::createChat() {
    Chat chat;
    chat.id = QUuid::createUuid().toString();
    chat.title = "New chat";
    chat.createdAt = QDateTime::currentDateTimeUtc();
    emit chatAboutToBeCreated(chatCount());
    chats_.push_back(chat);
    currentChatId_ = chat.id;
    emit chatCreated(chat.id, chat.title);
    emit currentChatChanged(chat.id);
    return chat.id;
}

const Chat* ChatManager::findChat(const QString& chatId) const {
    for (const auto& c : chats_) {
        if (c.id == chatId) {
            return &c;
        }
    }
    return nullptr;
}

Chat* ChatManager::findChat(const QString& chatId) {
    for (auto& c : chats_) {
        if (c.id == chatId) {
            return &c;
        }
    }
    return nullptr;
}

QString ChatManager::appendMessage(const QString& chatId, MessageRole role, const QString& text) {
    auto* chat = findChat(chatId);
    if (!chat) {
        return {};
    }
    const bool shouldSetTitle = chat->messages.empty() && role == MessageRole::User;
    ChatMessage msg{};
    msg.chatId = chatId;
    msg.id = QUuid::createUuid().toString();
    const QString messageId = msg.id;
    msg.role = role;
    msg.text = text;
    msg.createdAt = QDateTime::currentDateTimeUtc();
    chat->messages.push_back(std::move(msg));
    if (shouldSetTitle) {
        QString title = text.simplified();
        if (title.size() > 20) {
            title = title.left(20) + "...";
        }
        chat->title = title;
        emit chatTitleChanged(chatId);
    }

    emit messageAdded(chatId, messageId);
    return messageId;
}

bool ChatManager::selectChat(const QString& chatId) {
    const auto* chat = findChat(chatId);
    if (!chat) {
        return false;
    }
    if (currentChatId_ == chat->id) {
        return true;
    }
    currentChatId_ = chat->id;
    emit currentChatChanged(currentChatId_);
    return true;
}

int ChatManager::chatCount() const {
    return static_cast<int>(chats_.size());
}

const Chat* ChatManager::chatAt(int row) const {
    if (row < 0 || row >= chatCount()) {
        return nullptr;
    }
    return &chats_[row];
}

bool ChatManager::sendMessage(const QString& text) {
    if (text.trimmed().isEmpty()) {
        emit errorOccurred("Enter message");
        return false;
    }
    if (!pendingChatId_.isEmpty()) {
        emit errorOccurred("Wait answer on previous question");
        return false;
    }
    if (currentChatId_.isEmpty()) {
        createChat();
    }
    QString targetChatId = currentChatId_;
    QString id = appendMessage(targetChatId, MessageRole::User, text);
    if (id.isEmpty()) {
        emit errorOccurred("Cannot add message: chat is not find");
        return false;
    }
    pendingChatId_ = targetChatId;
    if (!service_->askQuestion(text)) {
        pendingChatId_.clear();
        emit errorOccurred("Service is busy. Question saved, but not sent");
        return false;
    }
    return true;
}

const Chat* ChatManager::currentChat() const {
    return findChat(currentChatId_);
}
