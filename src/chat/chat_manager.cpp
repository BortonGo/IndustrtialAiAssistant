#include "chat_manager.hpp"
#include <QUuid>
#include <QJsonArray>
#include <QTimer>

namespace {
QString newId() { return QUuid::createUuid().toString().mid(1, 36); }
QDateTime date(const QJsonValue& value) { return QDateTime::fromString(value.toString().replace(' ', 'T'), Qt::ISODate); }
QString titleFor(const QString& text) { const auto title = text.simplified(); return title.size() > 20 ? title.left(20) + "..." : title; }
}

ChatManager::ChatManager(AssistantService* service, QObject* parent) : QObject(parent), service_(service) {
    connect(service_->storage(), &StorageClient::ready, this, &ChatManager::restore);
    connect(service_, &AssistantService::answerReady, this, [this](const QString& answer) { finish(answer, {}); });
    connect(service_, &AssistantService::questionFailed, this, [this](const QString& error) { finish({}, error); });
    connect(service_, &AssistantService::contextSaved, this, [this](const QString& id, const QString& text) {
        if (pendingMessageId_ != id) return;
        auto* chat = findChat(pendingChatId_);
        if (!chat) return;
        for (auto& message : chat->messages) if (message.id == pendingMessageId_) message.contextSnapshot = text;
        emit messageAdded(pendingChatId_, pendingMessageId_);
    });
}

void ChatManager::restore() {
    operation_ = true; emit operationChanged();
    service_->storage()->request("bootstrap", {}, [this](const QJsonObject& data, const QString& error) {
        if (!error.isEmpty()) { operation_ = false; emit operationChanged(); emit errorOccurred(error); return; }
        emit chatsAboutToReset(); chats_.clear();
        for (const auto& value : data.value("chats").toArray()) {
            const auto object = value.toObject();
            Chat chat; chat.id = object.value("id").toString(); chat.title = object.value("title").toString(); chat.createdAt = date(object.value("created_at"));
            for (const auto& item : object.value("messages").toArray()) {
                const auto object = item.toObject();
                ChatMessage message;
                message.id = object.value("id").toString(); message.chatId = chat.id;
                message.role = object.value("role").toString() == "user" ? MessageRole::User : MessageRole::Assistant;
                message.text = object.value("text").toString(); message.createdAt = date(object.value("created_at"));
                message.status = object.value("status").toString(); message.error = object.value("error").toString();
                message.contextSnapshot = object.value("context_snapshot").toString();
                chat.messages.push_back(std::move(message));
            }
            chats_.push_back(std::move(chat));
        }
        emit chatsReset(); operation_ = false; emit operationChanged();
        selectChat(data.value("currentChatId").toString());
    });
}

QString ChatManager::createChat() {
    if (isBusy() || service_->isBusy() || !service_->storage()->isReady()) {
        emit errorOccurred(QStringLiteral("Дождитесь готовности БД и завершения текущей операции")); return {};
    }
    const auto id = newId(); operation_ = true; emit operationChanged();
    service_->storage()->request("create_chat", {{"id",id},{"title",QStringLiteral("Новый чат")}},
                                [this,id](const QJsonObject& data, const QString& error) {
        operation_ = false; emit operationChanged();
        if (!error.isEmpty()) { emit errorOccurred(error); return; }
        Chat chat; chat.id = id; chat.title = data.value("title").toString(); chat.createdAt = date(data.value("created_at"));
        emit chatAboutToBeCreated(chatCount()); chats_.push_back(chat); emit chatCreated(id, chat.title); selectChat(id);
    });
    return id;
}

const Chat* ChatManager::findChat(const QString& id) const { for (const auto& chat : chats_) if (chat.id == id) return &chat; return nullptr; }
Chat* ChatManager::findChat(const QString& id) { for (auto& chat : chats_) if (chat.id == id) return &chat; return nullptr; }
int ChatManager::chatCount() const { return static_cast<int>(chats_.size()); }
const Chat* ChatManager::chatAt(int row) const { return row >= 0 && row < chatCount() ? &chats_[row] : nullptr; }
const Chat* ChatManager::currentChat() const { return findChat(currentChatId_); }

bool ChatManager::selectChat(const QString& id) {
    if (!findChat(id)) return false;
    if (id == currentChatId_) return true;
    if (isBusy() || service_->isBusy()) { emit errorOccurred(QStringLiteral("Дождитесь завершения текущей операции перед сменой чата")); return false; }
    operation_ = true; emit operationChanged();
    service_->activateChat(id, [this,id](const QString& error) {
        operation_ = false; emit operationChanged();
        if (!error.isEmpty()) { emit errorOccurred(error); return; }
        currentChatId_ = id; emit currentChatChanged(id);
    });
    return true;
}

QString ChatManager::appendMessage(const QString& chatId, MessageRole role, const QString& text,
                                   const QString& id, const QString& status, const QString& error) {
    auto* chat = findChat(chatId); if (!chat) return {};
    ChatMessage message; message.id = id; message.chatId = chatId; message.role = role;
    message.text = text; message.createdAt = QDateTime::currentDateTimeUtc(); message.status = status; message.error = error;
    if (chat->messages.empty() && role == MessageRole::User) { chat->title = titleFor(text); emit chatTitleChanged(chatId); }
    chat->messages.push_back(std::move(message)); emit messageAdded(chatId, id); return id;
}

bool ChatManager::sendMessage(const QString& text) {
    if (text.trimmed().isEmpty() || isBusy() || service_->isBusy() || !service_->modelsReady() || currentChatId_.isEmpty()) {
        emit errorOccurred(QStringLiteral("Дождитесь готовности приложения и введите вопрос")); return false;
    }
    pendingChatId_ = currentChatId_; pendingMessageId_ = newId(); emit operationChanged();
    const auto chatId = pendingChatId_, messageId = pendingMessageId_;
    service_->storage()->request("save_question", {{"id",messageId},{"chatId",chatId},{"text",text},{"title",titleFor(text)}},
                                [this,chatId,messageId,text](const QJsonObject&, const QString& error) {
        if (!error.isEmpty()) {
            appendMessage(chatId, MessageRole::User, text, messageId, "unsaved", error);
            pendingChatId_.clear(); pendingMessageId_.clear(); emit operationChanged(); emit errorOccurred(error); return;
        }
        appendMessage(chatId, MessageRole::User, text, messageId, "pending");
        if (!service_->askQuestion(text, messageId)) finish({}, QStringLiteral("Сервис занят: вопрос сохранён, но не отправлен"));
    });
    return true;
}

void ChatManager::finish(const QString& answer, const QString& failure) {
    if (pendingChatId_.isEmpty()) return;
    const auto chatId = pendingChatId_, questionId = pendingMessageId_, answerId = newId();
    service_->storage()->request("finish_question", {{"questionId",questionId},{"id",answerId},{"text",answer},{"error",failure}},
                                [this,chatId,questionId,answerId,answer,failure](const QJsonObject&, const QString& error) {
        if (auto* chat = findChat(chatId)) for (auto& message : chat->messages) if (message.id == questionId) {
            message.status = error.isEmpty() ? (failure.isEmpty() ? "complete" : "failed") : "unsaved";
            message.error = error.isEmpty() ? failure : error;
        }
        if (!answer.isEmpty()) appendMessage(chatId, MessageRole::Assistant, answer, answerId, error.isEmpty() ? "complete" : "unsaved", error);
        else emit messageAdded(chatId, questionId);
        pendingChatId_.clear(); pendingMessageId_.clear(); emit operationChanged();
        if (!error.isEmpty() || !failure.isEmpty()) emit errorOccurred(error.isEmpty() ? failure : QStringLiteral("Не удалось сохранить результат: ") + error);
    });
}
