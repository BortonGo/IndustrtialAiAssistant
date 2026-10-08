#include "message_widget.hpp"
#include "markdown_render.hpp"
#include "attachment_card.hpp"
#include "ui_style.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QResizeEvent>
#include <QTextBrowser>
#include <QAbstractTextDocumentLayout>
#include <QScrollBar>
#include <QtMath>
#include <QToolButton>
#include <QApplication>
#include <QClipboard>

namespace {
// Keep wide code and tables inside the message instead of widening the chat.
// The document supplies the full height; only horizontal scrolling is local.
class AnswerTextBrowser final : public QTextBrowser {
    bool resizing_ = false;

    void updateHeight() {
        if (resizing_) return;
        resizing_ = true;
        const int chrome = height() - viewport()->height();
        const int needed = qMax(42, qCeil(document()->size().height()) + chrome);
        if (height() != needed) setFixedHeight(needed);
        resizing_ = false;
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QTextBrowser::resizeEvent(event);
        updateHeight();
    }

public:
    explicit AnswerTextBrowser(QWidget* parent = nullptr) : QTextBrowser(parent) {
        setObjectName("answerText");
        setReadOnly(true);
        setOpenLinks(false);
        setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        setFrameShape(QFrame::NoFrame);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        document()->setDocumentMargin(0);
        document()->setDefaultStyleSheet(QStringLiteral(
            "p { margin-top: 0px; margin-bottom: 14px; }"
            "h1, h2, h3 { font-weight: 600; margin-top: 12px; margin-bottom: 12px; }"
            "h1 { font-size: 23px; } h2 { font-size: 21px; } h3 { font-size: 19px; }"
            "th { color: #A5A6AA; font-weight: 500; text-align: left; }"
            "td, th { padding: 7px; } pre { background-color: #242527; }"));
        connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
                this, [this](const QSizeF&) { updateHeight(); });
    }
};
}

MessageWidget::MessageWidget(const ChatMessage& message, QWidget* parent) : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    const bool isUser = (message.role == MessageRole::User);
    QWidget* bubble = nullptr;
    if (isUser) {
        bubble_ = new QLabel;
        bubble_->setWordWrap(true);
        bubble_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        bubble = bubble_;
    } else {
        answer_ = new AnswerTextBrowser;
        bubble = answer_;
    }
    bubble->setMaximumWidth(isUser ? 640 : 900);

    const QString background = isUser ? "#242527" : "#191A1B";

    bubble->setStyleSheet(
            QString(
                "QLabel, QTextBrowser {"
                " background-color: %1;"
                " color: #ECECEE;"
                " border-radius: 14px;"
                " padding: %2;"
                " font-family: 'Segoe UI';"
                " font-size: 15px;"
                "}"
                "QScrollBar:horizontal { background: #191A1B; height: 8px; margin: 0; }"
                "QScrollBar::handle:horizontal { background: #454649; min-width: 30px; border-radius: 4px; }"
                "QScrollBar::handle:horizontal:hover { background: #626367; }"
                "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }"
                "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: none; }"
            ).arg(background, isUser ? "11px 16px" : "4px 0px")
        );
    QWidget* content = bubble;
    if (isUser && !message.attachments.empty()) {
        auto* group = new QWidget;
        group->setMaximumWidth(640);
        auto* column = new QVBoxLayout(group); column->setContentsMargins(0, 0, 0, 0); column->setSpacing(8);
        bubble->setMinimumWidth(280);
        column->addWidget(bubble);
        auto* fileGroup = new QWidget(group);
        auto* files = new QGridLayout(fileGroup); files->setSpacing(8); files->setContentsMargins(0, 0, 0, 0);
        files->setSizeConstraint(QLayout::SetFixedSize);
        for (int i = 0; i < static_cast<int>(message.attachments.size()); ++i)
            files->addWidget(createAttachmentCard(message.attachments[i], fileGroup), i / 2, i % 2);
        column->addWidget(fileGroup, 0, Qt::AlignRight); content = group;
    }
    if (isUser) {
        layout->addStretch();
        layout->addWidget(content);
    } else {
        auto* answerGroup = new QWidget;
        auto* column = new QVBoxLayout(answerGroup);
        column->setContentsMargins(0, 0, 0, 0); column->setSpacing(6);
        column->addWidget(bubble);
        auto* copy = new QToolButton;
        copy->setObjectName("copyAnswer"); copy->setIcon(UiStyle::icon(UiStyle::Icon::Copy));
        copy->setToolTip(QStringLiteral("Скопировать ответ")); copy->setAccessibleName(copy->toolTip());
        copy->setFixedSize(28, 28);
        column->addWidget(copy, 0, Qt::AlignLeft);
        connect(copy, &QToolButton::clicked, this, [this, copy]() {
            QApplication::clipboard()->setText(answer_->toPlainText());
            copy->setToolTip(QStringLiteral("Ответ скопирован"));
        });
        layout->addWidget(answerGroup, 1);
    }
    setMessage(message);
}

void MessageWidget::setMessage(const ChatMessage& message) {
    if (message.role == MessageRole::User) {
        bubble_->setTextFormat(Qt::PlainText);
        auto text = message.text;
        if (message.status == "failed") text += QStringLiteral("\n\nОшибка: ") + message.error;
        if (message.status == "unsaved") text += QStringLiteral("\n\nНе сохранено: ") + message.error;
        bubble_->setText(text);
    } else {
        QString note;
        if (message.status == "failed") note = QStringLiteral("Ответ прерван: ");
        if (message.status == "unsaved") note = QStringLiteral("Не сохранено: ");
        QString html = markdownToHtml(message.text);
        html.replace("<table>", "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"7\">");
        html.replace("<th>", "<th align=\"left\">");
        answer_->setHtml(html + (note.isEmpty() ? QString() :
            "<p><b>" + note + "</b>" + message.error.toHtmlEscaped() + "</p>"));
    }
    updateMinimumHeight();
}

void MessageWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (event->size().width() != event->oldSize().width()) updateMinimumHeight();
}

void MessageWidget::updateMinimumHeight() {
    if (!layout() || layout()->isEmpty()) return;
    // Qt 5's scroll area can otherwise compress wrapped text to its old size hint.
    layout()->invalidate();
    const int needed = qMax(0, layout()->totalHeightForWidth(width()));
    if (minimumHeight() != needed) setMinimumHeight(needed);
}
