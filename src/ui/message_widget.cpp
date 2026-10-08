#include "message_widget.hpp"
#include "markdown_render.hpp"

#include <QHBoxLayout>
#include <QLabel>

MessageWidget::MessageWidget(const ChatMessage& message, QWidget* parent) : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    const bool isUser = (message.role == MessageRole::User);
    auto* bubble = new QLabel;
    if (isUser) {
        bubble->setTextFormat(Qt::PlainText);
        QString text = message.text;
        if (message.status == "pending") text += QStringLiteral("\n\nОжидание ответа…");
        if (message.status == "failed") text += QStringLiteral("\n\nОшибка: ") + message.error;
        if (message.status == "unsaved") text += QStringLiteral("\n\nНе сохранено: ") + message.error;
        bubble->setText(text);
    } else {
        bubble->setTextFormat(Qt::RichText);
        bubble->setText(markdownToHtml(message.text) + (message.status == "unsaved" ?
                           QStringLiteral("<p><b>Не сохранено:</b> ") + message.error.toHtmlEscaped() + "</p>" : QString()));
    }
    bubble->setWordWrap(true);
    bubble->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bubble->setMaximumWidth(640);

    const QString background = isUser ? "#263D60" : "#24262B";

    bubble->setStyleSheet(
            QString(
                "QLabel {"
                " background-color: %1;"
                " color: #ECEEF2;"
                " border-radius: 14px;"
                " padding: 12px 16px;"
                " font-family: 'Segoe UI';"
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
