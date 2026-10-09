#pragma once

#include "chat/chat.hpp"
#include <QWidget>
class QLabel;
class QTextBrowser;
class QResizeEvent;
class QToolButton;

class MessageWidget : public QWidget {
    Q_OBJECT
    QLabel* bubble_ = nullptr;
    QTextBrowser* answer_ = nullptr;
    QToolButton* sources_ = nullptr;
    void updateMinimumHeight();
protected:
    void resizeEvent(QResizeEvent* event) override;
public:
    explicit MessageWidget(const ChatMessage& message, QWidget* parent = nullptr);
    void setMessage(const ChatMessage& message);
    void setSourcesAvailable(bool available, int count);
signals:
    void sourcesRequested();
};
