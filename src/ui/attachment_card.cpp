#include "attachment_card.hpp"
#include <QFrame>
#include <QLabel>
#include <QToolButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QImageReader>
#include <QFileInfo>
#include <QDialog>
#include <QPushButton>

namespace {
QPixmap preview(const QString& path, const QSize& bounds) {
    QImageReader reader(path); reader.setAutoTransform(true);
    if (reader.size().isValid()) reader.setScaledSize(reader.size().scaled(bounds, Qt::KeepAspectRatio));
    return QPixmap::fromImage(reader.read()).scaled(bounds, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}
}

QWidget* createAttachmentCard(const MessageAttachment& attachment, QWidget* parent, std::function<void()> remove) {
    auto* card = new QFrame(parent);
    card->setObjectName("attachmentCard"); card->setFixedSize(176, 80);
    card->setStyleSheet("QFrame#attachmentCard { background: #303746; border: 1px solid #46516A; border-radius: 10px; }"
                       "QLabel { border: none; background: transparent; color: #E6EBF5; font-size: 12px; padding: 0px; }"
                       "QToolButton { border: none; background: transparent; color: #B8C4D9; font-size: 16px; }"
                       "QToolButton:hover { color: white; background: #46516A; border-radius: 5px; }");
    card->setToolTip(attachment.fileName);
    auto* row = new QHBoxLayout(card); row->setContentsMargins(8, 8, 6, 8); row->setSpacing(6);
    const auto extension = QFileInfo(attachment.sourcePath).suffix().toLower();
    const bool picture = QStringList{"png","jpg","jpeg","webp","bmp"}.contains(extension);
    const auto imagePath = attachment.previewPath.isEmpty() ? attachment.sourcePath : attachment.previewPath;
    const auto thumbnail = picture ? preview(imagePath, QSize(52, 52)) : QPixmap();
    auto* icon = new QToolButton(card); icon->setFixedSize(52, 56);
    if (!thumbnail.isNull()) {
        icon->setIcon(QIcon(thumbnail)); icon->setIconSize(QSize(52, 52));
        icon->setToolTip(QStringLiteral("Открыть изображение"));
        QObject::connect(icon, &QToolButton::clicked, card, [card, imagePath, attachment]() {
            QDialog dialog(card); dialog.setWindowTitle(attachment.fileName);
            dialog.setStyleSheet("QDialog { background: #18191C; } QLabel { color: #E6EBF5; }");
            auto* layout = new QVBoxLayout(&dialog);
            auto* image = new QLabel(&dialog); image->setAlignment(Qt::AlignCenter);
            const auto pixels = preview(imagePath, QSize(900, 650));
            if (pixels.isNull()) image->setText(QStringLiteral("Изображение недоступно")); else image->setPixmap(pixels);
            layout->addWidget(image);
            auto* close = new QPushButton(QStringLiteral("Закрыть"), &dialog);
            QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
            layout->addWidget(close); dialog.exec();
        });
    } else {
        icon->setText(extension.toUpper().left(4)); icon->setEnabled(false);
    }
    row->addWidget(icon);
    auto* details = new QVBoxLayout; details->setSpacing(4);
    auto* name = new QLabel; name->setTextFormat(Qt::PlainText); name->setWordWrap(true);
    name->setText(name->fontMetrics().elidedText(attachment.fileName, Qt::ElideMiddle, remove ? 79 : 100));
    auto* type = new QLabel(picture ? QStringLiteral("Изображение") : QStringLiteral("Документ"));
    type->setStyleSheet("color: #A4B1C9; font-size: 10px;");
    details->addWidget(name); details->addWidget(type); row->addLayout(details, 1);
    if (remove) {
        auto* close = new QToolButton(card); close->setText(QString::fromUtf8("×")); close->setFixedSize(18, 22);
        close->setToolTip(QStringLiteral("Убрать вложение"));
        QObject::connect(close, &QToolButton::clicked, card, [remove]() { remove(); });
        row->addWidget(close, 0, Qt::AlignTop);
    }
    return card;
}
