#pragma once

#include "chat/chat.hpp"
#include <QWidget>
class QLabel;
class QTextBrowser;
class QResizeEvent;

class MessageWidget : public QWidget {
    QLabel* bubble_ = nullptr;
    QTextBrowser* answer_ = nullptr;
    void updateMinimumHeight();
protected:
    void resizeEvent(QResizeEvent* event) override;
public:
    explicit MessageWidget(const ChatMessage& message, QWidget* parent = nullptr);
    void setMessage(const ChatMessage& message);
};
