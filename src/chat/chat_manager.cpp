#include "chat_manager.hpp"
#include <QUuid>
#include <QJsonArray>
#include <QTimer>
#include <QFileInfo>
#include <algorithm>

namespace {
QString newId() { return QUuid::createUuid().toString().mid(1, 36); }
QDateTime date(const QJsonValue& value) { return QDateTime::fromString(value.toString().replace(' ', 'T'), Qt::ISODate); }
QString titleFor(const QString& text) { const auto title = text.simplified(); return title.size() > 20 ? title.left(20) + "..." : title; }
std::vector<MessageAttachment> attachmentsFromJson(const QJsonArray& values) {
    std::vector<MessageAttachment> result;
    for (const auto& value : values) {
        const auto object = value.toObject();
        result.push_back({object.value("fileName").toString(), object.value("sourcePath").toString(),
                          object.value("documentId").toString(), object.value("previewPath").toString()});
    }
    return result;
}
}

ChatManager::ChatManager(AssistantService* service, QObject* parent) : QObject(parent), service_(service) {
    connect(service_, &AssistantService::busyChanged, this, [this]() { emit chatActivityChanged(); });
    connect(this, &ChatManager::operationChanged, this, &ChatManager::chatActivityChanged);
    connect(service_->storage(), &StorageClient::ready, this, &ChatManager::restore);
    connect(service_, &AssistantService::answerReady, this, [this](const QString& answer) { finish(answer, {}); });
    connect(service_, &AssistantService::answerProgress, this, [this](const QString& id, const QString& text) {
        if (id != pendingMessageId_ || pendingChatId_.isEmpty() || finishing_) return;
        const bool first = pendingAnswer_.isEmpty();
        pendingAnswer_ = text;
        emit answerProgress(pendingChatId_, text);
        if (first) emit chatActivityChanged();
    });
    connect(service_, &AssistantService::questionFailed, this, [this](const QString& error) { finish({}, error); });
    connect(service_, &AssistantService::contextSaved, this, [this](const QString& id, const QString& text) {
        if (pendingMessageId_ != id) return;
        auto* chat = findChat(pendingChatId_);
        if (!chat) return;
        for (auto& message : chat->messages) if (message.id == pendingMessageId_) message.contextSnapshot = text;
        emit messageAdded(pendingChatId_, pendingMessageId_);
    });
    connect(service_, &AssistantService::attachmentsSaved, this, [this](const QString& id, const QJsonArray& values) {
        if (id != pendingMessageId_) return;
        if (auto* chat = findChat(pendingChatId_)) for (auto& message : chat->messages) if (message.id == id) {
            message.attachments = attachmentsFromJson(values); emit messageAdded(chat->id, id); break;
        }
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
                message.attachments = attachmentsFromJson(object.value("attachments").toArray());
                chat.messages.push_back(std::move(message));
            }
            chats_.push_back(std::move(chat));
        }
        emit chatsReset(); operation_ = false; emit operationChanged();
        selectChat(data.value("currentChatId").toString());
    });
}

QString ChatManager::createChat() {
    if (operation_ || !service_->storage()->isReady()) {
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
    if (!findChat(id) || id == deletingChatId_) return false;
    if (id == currentChatId_ && service_->isChatReady(id)) return true;
    currentChatId_ = id;
    service_->activateChat(id, [this,id](const QString& error) {
        if (!error.isEmpty() && currentChatId_ == id) emit errorOccurred(error);
        emit operationChanged();
    });
    emit currentChatChanged(id);
    return true;
}

QString ChatManager::chatActivity(const QString& id) const {
    if (id == deletingChatId_) return QStringLiteral("Удаление…");
    if (id == pendingChatId_) return pendingAnswer_.isEmpty() ? QStringLiteral("Готовится ответ…") : QStringLiteral("Пишет ответ…");
    if (id == service_->operationChatId()) return QStringLiteral("Обработка…");
    return {};
}

bool ChatManager::canDeleteChat(const QString& id) const {
    return findChat(id) && !operation_ && service_->storage()->isReady() &&
            id != pendingChatId_ && id != service_->operationChatId();
}

bool ChatManager::deleteChat(const QString& id) {
    if (!canDeleteChat(id)) {
        emit errorOccurred(QStringLiteral("Этот чат сейчас занят. Дождитесь завершения операции."));
        return false;
    }
    deletingChatId_ = id; operation_ = true; emit operationChanged();
    service_->storage()->request("delete_chat", {{"chatId", id}, {"currentChatId", currentChatId_}},
                                [this,id](const QJsonObject& data, const QString& error) {
        if (!error.isEmpty()) {
            deletingChatId_.clear(); operation_ = false; emit operationChanged(); emit errorOccurred(error); return;
        }
        const auto it = std::find_if(chats_.begin(), chats_.end(), [id](const Chat& chat) { return chat.id == id; });
        if (it != chats_.end()) {
            emit chatAboutToBeRemoved(static_cast<int>(it - chats_.begin()));
            chats_.erase(it);
            emit chatRemoved(id);
        }
        service_->forgetChat(id);
        const auto replacement = data.value("replacementChat").toObject();
        if (!replacement.isEmpty()) {
            Chat chat; chat.id = replacement.value("id").toString(); chat.title = replacement.value("title").toString();
            chat.createdAt = date(replacement.value("created_at"));
            emit chatAboutToBeCreated(chatCount()); chats_.push_back(chat); emit chatCreated(chat.id, chat.title);
        }
        deletingChatId_.clear(); operation_ = false;
        // Navigation may have changed while PostgreSQL was answering.
        if (currentChatId_ == id || !findChat(currentChatId_)) {
            currentChatId_.clear();
            selectChat(data.value("currentChatId").toString());
        }
        emit operationChanged();
        QStringList warnings;
        for (const auto& value : data.value("warnings").toArray()) warnings.append(value.toString());
        emit notification(warnings.isEmpty() ? QStringLiteral("Чат и его документы удалены") :
                          QStringLiteral("Чат удалён. ") + warnings.join("\n"));
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

bool ChatManager::sendMessage(const QString& input, const QStringList& paths) {
    if ((input.trimmed().isEmpty() && paths.isEmpty()) || isBusy() || service_->isBusy() || !service_->modelsReady() || !service_->isChatReady(currentChatId_)) {
        emit errorOccurred(QStringLiteral("Дождитесь готовности приложения и введите вопрос")); return false;
    }
    if (paths.size() > 4) { emit errorOccurred(QStringLiteral("Можно прикрепить до четырёх файлов")); return false; }
    QStringList attachments;
    for (const auto& path : paths) {
        const QFileInfo file(path);
        if (!file.isFile() || !file.isReadable()) { emit errorOccurred(QStringLiteral("Файл недоступен: ") + path); return false; }
        const auto canonical = file.canonicalFilePath();
        if (!attachments.contains(canonical, Qt::CaseInsensitive)) attachments.append(canonical);
    }
    const auto text = input.trimmed().isEmpty() ? QStringLiteral("Расскажи, что содержится в прикреплённых файлах.") : input;
    pendingAnswer_.clear();
    pendingChatId_ = currentChatId_; pendingMessageId_ = newId(); emit operationChanged();
    const auto chatId = pendingChatId_, messageId = pendingMessageId_;
    service_->storage()->request("save_question", {{"id",messageId},{"chatId",chatId},{"text",text},{"title",titleFor(text)},
                                                 {"attachments",QJsonArray::fromStringList(attachments)}},
                                [this,chatId,messageId,text,attachments](const QJsonObject&, const QString& error) {
        if (!error.isEmpty()) {
            appendMessage(chatId, MessageRole::User, text, messageId, "unsaved", error);
            if (auto* chat = findChat(chatId)) {
                for (const auto& path : attachments) chat->messages.back().attachments.push_back({QFileInfo(path).fileName(),path,{},{}});
                if (!attachments.isEmpty()) emit messageAdded(chatId, messageId);
            }
            pendingChatId_.clear(); pendingMessageId_.clear(); emit operationChanged(); emit errorOccurred(error); return;
        }
        appendMessage(chatId, MessageRole::User, text, messageId, "pending");
        if (auto* chat = findChat(chatId)) {
            for (const auto& path : attachments) chat->messages.back().attachments.push_back({QFileInfo(path).fileName(),path,{},{}});
            if (!attachments.isEmpty()) emit messageAdded(chatId, messageId);
        }
        if (!service_->askQuestion(text, messageId, chatId, attachments)) finish({}, QStringLiteral("Сервис занят: вопрос сохранён, но не отправлен"));
    });
    return true;
}

void ChatManager::finish(const QString& result, const QString& failure) {
    if (pendingChatId_.isEmpty() || finishing_) return;
    finishing_ = true;
    const auto answer = failure.isEmpty() ? result : pendingAnswer_;
    const auto chatId = pendingChatId_, questionId = pendingMessageId_, answerId = newId();
    service_->storage()->request("finish_question", {{"questionId",questionId},{"id",answerId},{"text",answer},{"error",failure}},
                                [this,chatId,questionId,answerId,answer,failure](const QJsonObject&, const QString& error) {
        if (auto* chat = findChat(chatId)) for (auto& message : chat->messages) if (message.id == questionId) {
            message.status = error.isEmpty() ? (failure.isEmpty() ? "complete" : "failed") : "unsaved";
            message.error = error.isEmpty() ? failure : error;
        }
        pendingAnswer_.clear();
        if (!answer.isEmpty()) appendMessage(chatId, MessageRole::Assistant, answer, answerId,
            error.isEmpty() ? (failure.isEmpty() ? "complete" : "failed") : "unsaved", error.isEmpty() ? failure : error);
        else emit messageAdded(chatId, questionId);
        pendingChatId_.clear(); pendingMessageId_.clear(); finishing_ = false; emit operationChanged();
        if (!error.isEmpty() || !failure.isEmpty()) emit errorOccurred(error.isEmpty() ? failure : QStringLiteral("Не удалось сохранить результат: ") + error);
    });
}
