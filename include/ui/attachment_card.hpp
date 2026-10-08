#pragma once
#include "chat/chat.hpp"
#include <functional>
class QWidget;

QWidget* createAttachmentCard(const MessageAttachment& attachment, QWidget* parent,
                              std::function<void()> remove = {});
