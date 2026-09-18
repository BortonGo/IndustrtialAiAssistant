#pragma once

#include <QString>
#include <QDateTime>

#include <vector>

enum class MessageRole {
    User,
    Assistant
};

struct ChatMessage final {
    QString id;
    QString chatId;
    MessageRole role = MessageRole::User;
    QString text;
    QDateTime createdAt;
};

struct Chat final {
    QString id;
    QString title;
    QDateTime createdAt;
    std::vector<ChatMessage> messages;
};
