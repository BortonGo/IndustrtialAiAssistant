#include "message_composer.hpp"
#include "attachment_card.hpp"
#include "ui_style.hpp"
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
#include <QKeyEvent>
#include <QtMath>
#include <QLabel>
#include <QMimeData>
#include <QImage>
#include <QPixmap>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QSaveFile>
#include <QStandardPaths>
#include <functional>
#include <utility>

namespace {
// Keep the empty composer compact; scroll only after a few lines of text.
class ComposerTextEdit : public QTextEdit {
    std::function<void(const QImage&)> pasteImage_;
    std::function<void()> submit_;
public:
    ComposerTextEdit(std::function<void(const QImage&)> pasteImage, std::function<void()> submit)
        : pasteImage_(std::move(pasteImage)), submit_(std::move(submit)) {
        setAcceptRichText(false);
        setFixedHeight(32);
        connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
                this, [this](const QSizeF&) { updateHeight(); });
    }
protected:
    void keyPressEvent(QKeyEvent* event) override {
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
            && !(event->modifiers() & Qt::ShiftModifier)) {
            if (!event->isAutoRepeat()) submit_();
            event->accept();
            return;
        }
        QTextEdit::keyPressEvent(event);
    }
    bool canInsertFromMimeData(const QMimeData* source) const override {
        return source->hasImage() || QTextEdit::canInsertFromMimeData(source);
    }
    void insertFromMimeData(const QMimeData* source) override {
        if (source->hasImage()) {
            const auto data = source->imageData();
            pasteImage_(data.userType() == QMetaType::QPixmap
                        ? qvariant_cast<QPixmap>(data).toImage() : qvariant_cast<QImage>(data));
            return;
        }
        QTextEdit::insertFromMimeData(source);
    }
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
        " background-color: #28292B;"
        " border: 1px solid #363739;"
        " border-radius: 18px;"
        "}"
    );

    textQuestion_ = new ComposerTextEdit([this](const QImage& image) { pasteImage(image); },
                                       [this]() { sendButton_->click(); });
    textQuestion_->setObjectName("messageInput");
    textQuestion_->setToolTip(QStringLiteral("Enter — отправить · Shift+Enter — новая строка\nCtrl+V — вставить текст или скриншот"));

    textQuestion_->setPlaceholderText(QStringLiteral("Спросите что-нибудь или прикрепите файл…"));
    textQuestion_->setStyleSheet(
        "QTextEdit {"
        " background-color: transparent;"
        " color: #ECECEE;"
        " border: none;"
        " padding: 0px;"
        " font-size: 15px;"
        "}"
    );

    QPushButton *btnAsk = new QPushButton;
    btnAsk->setObjectName("sendMessageButton");
    btnAsk->setIcon(UiStyle::icon(UiStyle::Icon::Send, QColor("#191A1B")));
    btnAsk->setIconSize(QSize(20, 20));
    sendButton_ = btnAsk;
    btnAsk->setFixedSize(32,32);
    btnAsk->setToolTip(QStringLiteral("Отправить сообщение (Enter)"));
    btnAsk->setAccessibleName(btnAsk->toolTip());
    btnAsk->setStyleSheet(
                "QPushButton {"
                " background-color: #ECECEE;"
                " color: #191A1B;"
                " border: none;"
                " border-radius: 16px;"
                " padding: 0px;"
                " font-size: 24px;"
                "}"
                "QPushButton:hover { background-color: #CCD7E8; }"
                "QPushButton:disabled {"
                " background-color: #454649;"
                " color: #84858A;"
                "}"
                );

    QPushButton *btnLoadDoc = new QPushButton;
    btnLoadDoc->setObjectName("attachFileButton");
    btnLoadDoc->setIcon(UiStyle::icon(UiStyle::Icon::Plus));
    btnLoadDoc->setIconSize(QSize(20, 20));
    uploadButton_ = btnLoadDoc;
    btnLoadDoc->setFixedSize(32,32);
    btnLoadDoc->setToolTip("Прикрепить файл к сообщению");
    btnLoadDoc->setAccessibleName(btnLoadDoc->toolTip());
    btnLoadDoc->setStyleSheet(
                "QPushButton {"
                " background-color: transparent;"
                " color: #A5A6AA;"
                " border: none;"
                " border-radius: 7px;"
                " padding: 0px;"
                " font-size: 24px;"
                "}"
                "QPushButton:hover { background-color: #363739; }"
                );

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 10, 14, 10);
    layout->setSpacing(6);

    auto* buttonsLayout = new QHBoxLayout;
    buttonsLayout->setContentsMargins(0, 0, 0, 0);
    buttonsLayout->setSpacing(8);

    buttonsLayout->addWidget(btnLoadDoc);
    auto* modelLabel = new QLabel(QStringLiteral("Локальная модель"));
    modelLabel->setStyleSheet("color: #A5A6AA; font-size: 11px;");
    buttonsLayout->addWidget(modelLabel);
    buttonsLayout->addStretch();
    buttonsLayout->addWidget(btnAsk);

    attachmentsArea_ = new QScrollArea(this);
    attachmentsArea_->setObjectName("draftAttachments");
    attachmentsArea_->setWidgetResizable(true); attachmentsArea_->setFrameShape(QFrame::NoFrame);
    attachmentsArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    attachmentsArea_->setFixedHeight(100);
    attachmentsArea_->setStyleSheet("QScrollArea { border: none; background: transparent; }");
    attachmentsArea_->viewport()->setObjectName("attachmentViewport");
    attachmentsArea_->viewport()->setStyleSheet("QWidget#attachmentViewport { background: #28292B; }");
    auto* attachments = new QWidget;
    attachments->setObjectName("attachmentCanvas");
    attachments->setStyleSheet("QWidget#attachmentCanvas { background: #28292B; }");
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

void MessageComposer::addAttachmentPaths(const QStringList& paths) {
    auto combined = attachmentPaths_;
    for (const auto& path : paths)
        if (!combined.contains(path, Qt::CaseInsensitive)) combined.append(path);
    if (combined.size() > 4) {
        emit attachmentError(QStringLiteral("К одному сообщению можно прикрепить до четырёх файлов."));
        return;
    }
    setAttachmentPaths(combined);
}

void MessageComposer::pasteImage(const QImage& image) {
    if (!uploadButton_->isEnabled()) {
        emit attachmentError(QStringLiteral("Прикрепление изображений пока недоступно. Дождитесь завершения текущей операции и готовности сервисов."));
        return;
    }
    if (attachmentPaths_.size() >= 4) {
        emit attachmentError(QStringLiteral("К одному сообщению можно прикрепить до четырёх файлов."));
        return;
    }
    QByteArray png;
    QBuffer buffer(&png);
    // QPixmap may drop the alpha channel of an opaque screenshot. Normalize before hashing.
    if (image.isNull() || !buffer.open(QIODevice::WriteOnly)
        || !image.convertToFormat(QImage::Format_ARGB32).save(&buffer, "PNG")) {
        emit attachmentError(QStringLiteral("Не удалось прочитать изображение из буфера обмена."));
        return;
    }
    const auto cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    const auto directory = QDir(cache).filePath("clipboard-images");
    if (cache.isEmpty() || !QDir().mkpath(directory)) {
        emit attachmentError(QStringLiteral("Не удалось создать папку для скриншотов."));
        return;
    }
    // Keep the source beyond draft changes and asynchronous indexing. Identical pastes reuse it.
    const auto hash = QCryptographicHash::hash(png, QCryptographicHash::Sha256).toHex();
    const auto path = QDir(directory).filePath("screenshot-" + QString::fromLatin1(hash) + ".png");
    if (!QFileInfo::exists(path)) {
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(png) != png.size() || !file.commit()) {
            emit attachmentError(QStringLiteral("Не удалось сохранить скриншот: %1").arg(file.errorString()));
            return;
        }
    }
    addAttachmentPaths({path});
}

void MessageComposer::setAttachmentPaths(const QStringList& paths) {
    const auto revision = ++attachmentRevision_;
    attachmentPaths_ = paths;
    while (auto* item = attachmentsLayout_->takeAt(0)) { delete item->widget(); delete item; }
    int cardHeight = 44;
    for (const auto& path : paths) {
        MessageAttachment attachment{QFileInfo(path).fileName(), path, {}, {}};
        auto* card = createAttachmentCard(attachment, attachmentsArea_, [this, path, revision]() {
            // Let the clicked button finish delivering its signal before deleting its card.
            QTimer::singleShot(0, this, [this, path, revision]() {
                if (revision != attachmentRevision_) return;
                auto remaining = attachmentPaths_; remaining.removeAll(path); setAttachmentPaths(remaining);
            });
        });
        cardHeight = qMax(cardHeight, card->height());
        attachmentsLayout_->addWidget(card);
    }
    attachmentsLayout_->addStretch();
    attachmentsArea_->setFixedHeight(cardHeight + 16);
    attachmentsArea_->setVisible(!paths.isEmpty());
}

void MessageComposer::setSendEnabled(bool enabled) { sendButton_->setEnabled(enabled); }
QString MessageComposer::inputText() const { return textQuestion_->toPlainText(); }
void MessageComposer::setInputText(const QString& text) { textQuestion_->setPlainText(text); }
void MessageComposer::setUploadEnabled(bool enabled) { uploadButton_->setEnabled(enabled); }
