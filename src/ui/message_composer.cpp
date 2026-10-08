#include "message_composer.hpp"
#include "attachment_card.hpp"
#include <QScrollArea>
#include <QFileInfo>
#include <QTimer>

#include <QTextEdit>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QAbstractTextDocumentLayout>
#include <QTextDocument>
#include <QResizeEvent>
#include <QtMath>

namespace {
// Keep the empty composer compact; scroll only after a few lines of text.
class ComposerTextEdit : public QTextEdit {
public:
    ComposerTextEdit() {
        setAcceptRichText(false);
        setFixedHeight(32);
        connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
                this, [this](const QSizeF&) { updateHeight(); });
    }
protected:
    void resizeEvent(QResizeEvent* event) override {
        QTextEdit::resizeEvent(event);
        updateHeight();
    }
private:
    void updateHeight() {
        const int desired = qBound(32, qCeil(document()->documentLayout()->documentSize().height()) + 2, 132);
        if (height() != desired) setFixedHeight(desired);
    }
};
}

MessageComposer::MessageComposer(QWidget* parent) : QWidget(parent) {
    setObjectName("messageComposer");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(
        "QWidget#messageComposer {"
        " background-color: #24262B;"
        " border: 1px solid #393C44;"
        " border-radius: 18px;"
        "}"
    );

    textQuestion_ = new ComposerTextEdit;

    textQuestion_->setPlaceholderText(QStringLiteral("Напишите вопрос или прикрепите файл…"));
    textQuestion_->setStyleSheet(
        "QTextEdit {"
        " background-color: transparent;"
        " color: #E8E8ED;"
        " border: none;"
        " padding: 0px;"
        " font-size: 16px;"
        "}"
    );

    QPushButton *btnAsk = new QPushButton("↑");
    sendButton_ = btnAsk;
    btnAsk->setFixedSize(36,36);
    btnAsk->setToolTip("Send question");
    btnAsk->setStyleSheet(
                "QPushButton {"
                " background-color: #1E90FF;"
                " color: #E8E8ED;"
                " border: none;"
                " border-radius: 18px;"
                " padding: 0px;"
                " padding-bottom: 4px;"
                " font-size: 24px;"
                "}"
                "QPushButton:hover { background-color: #46A3FF; }"
                "QPushButton:disabled {"
                " background-color: #393C44;"
                " color: #777B85;"
                "}"
                );

    QPushButton *btnLoadDoc = new QPushButton("+");
    uploadButton_ = btnLoadDoc;
    btnLoadDoc->setFixedSize(36,36);
    btnLoadDoc->setToolTip("Прикрепить файл к сообщению");
    btnLoadDoc->setStyleSheet(
                "QPushButton {"
                " background-color: #393C44;"
                " color: #E8E8ED;"
                " border: none;"
                " border-radius: 18px;"
                " padding: 0px;"
                " padding-bottom: 4px;"
                " font-size: 24px;"
                "}"
                "QPushButton:hover { background-color: #494D57; }"
                );

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 10, 14, 10);
    layout->setSpacing(6);

    auto* buttonsLayout = new QHBoxLayout;
    buttonsLayout->setContentsMargins(0, 0, 0, 0);
    buttonsLayout->setSpacing(8);

    buttonsLayout->addWidget(btnLoadDoc);
    buttonsLayout->addStretch();
    buttonsLayout->addWidget(btnAsk);

    attachmentsArea_ = new QScrollArea(this);
    attachmentsArea_->setObjectName("draftAttachments");
    attachmentsArea_->setWidgetResizable(true); attachmentsArea_->setFrameShape(QFrame::NoFrame);
    attachmentsArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    attachmentsArea_->setFixedHeight(100);
    attachmentsArea_->setStyleSheet("QScrollArea { border: none; background: transparent; }");
    attachmentsArea_->viewport()->setObjectName("attachmentViewport");
    attachmentsArea_->viewport()->setStyleSheet("QWidget#attachmentViewport { background: #24262B; }");
    auto* attachments = new QWidget;
    attachments->setObjectName("attachmentCanvas");
    attachments->setStyleSheet("QWidget#attachmentCanvas { background: #24262B; }");
    attachmentsLayout_ = new QHBoxLayout(attachments);
    attachmentsLayout_->setContentsMargins(0, 0, 0, 0); attachmentsLayout_->setSpacing(8);
    attachmentsArea_->setWidget(attachments); attachmentsArea_->hide();
    layout->addWidget(attachmentsArea_);
    layout->addWidget(textQuestion_);
    layout->addLayout(buttonsLayout);

    connect(btnLoadDoc, &QPushButton::clicked,
            this, &MessageComposer::documentUploadRequested);

    connect(btnAsk, &QPushButton::clicked,
            this, [this]() {
        emit sendRequested(textQuestion_->toPlainText());
    });
}

void MessageComposer::clearInput() {
    textQuestion_->clear();
    setAttachmentPaths({});
}

void MessageComposer::setAttachmentPaths(const QStringList& paths) {
    const auto revision = ++attachmentRevision_;
    attachmentPaths_ = paths;
    while (auto* item = attachmentsLayout_->takeAt(0)) { delete item->widget(); delete item; }
    for (const auto& path : paths) {
        MessageAttachment attachment{QFileInfo(path).fileName(), path, {}, {}};
        auto* card = createAttachmentCard(attachment, attachmentsArea_, [this, path, revision]() {
            // Let the clicked button finish delivering its signal before deleting its card.
            QTimer::singleShot(0, this, [this, path, revision]() {
                if (revision != attachmentRevision_) return;
                auto remaining = attachmentPaths_; remaining.removeAll(path); setAttachmentPaths(remaining);
            });
        });
        attachmentsLayout_->addWidget(card);
    }
    attachmentsLayout_->addStretch();
    attachmentsArea_->setVisible(!paths.isEmpty());
}

void MessageComposer::setSendEnabled(bool enabled) { sendButton_->setEnabled(enabled); }
QString MessageComposer::inputText() const { return textQuestion_->toPlainText(); }
void MessageComposer::setInputText(const QString& text) { textQuestion_->setPlainText(text); }
void MessageComposer::setUploadEnabled(bool enabled) { uploadButton_->setEnabled(enabled); }
