#pragma once

#include <QString>
#include <QDateTime>

#include <vector>

enum class MessageRole {
    User,
    Assistant
};

struct MessageAttachment {
    QString fileName;
    QString sourcePath;
    QString documentId;
    QString previewPath;
};

struct ChatMessage final {
    QString id;
    QString chatId;
    MessageRole role = MessageRole::User;
    QString text;
    QDateTime createdAt;
    QString status = "complete";
    QString error;
    QString contextSnapshot;
    std::vector<MessageAttachment> attachments;
};

struct Chat final {
    QString id;
    QString title;
    QDateTime createdAt;
    std::vector<ChatMessage> messages;
};
