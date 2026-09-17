#pragma once

#include "chat/chat.hpp"
#include <QWidget>

class MessageWidget : public QWidget {
public:
    explicit MessageWidget(const ChatMessage& message, QWidget* parent = nullptr);
};
