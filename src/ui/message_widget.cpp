#include "message_widget.hpp"

#include <QHBoxLayout>
#include <QLabel>

MessageWidget::MessageWidget(const ChatMessage& message, QWidget* parent) : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* bubble = new QLabel(message.text);
    bubble->setTextFormat(Qt::PlainText);
    bubble->setWordWrap(true);
    bubble->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bubble->setMaximumWidth(640);

    const bool isUser = (message.role == MessageRole::User);
    const QString background = isUser ? "#263D60" : "#24262B";

    bubble->setStyleSheet(
            QString(
                "QLabel {"
                " background-color: %1;"
                " color: #ECEEF2;"
                " border-radius: 14px;"
                " padding: 12px 16px;"
                " font-size: 16px;"
                "}"
            ).arg(background)
        );
    if (isUser) {
        layout->addStretch();
        layout->addWidget(bubble);
    } else {
        layout->addWidget(bubble);
        layout->addStretch();
    }
}
