#include "attachment_card.hpp"
#include "ui_style.hpp"
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
    const auto extension = QFileInfo(attachment.sourcePath).suffix().toLower();
    const bool picture = QStringList{"png","jpg","jpeg","webp","bmp"}.contains(extension);
    auto* card = new QFrame(parent);
    card->setObjectName("attachmentCard"); card->setFixedSize(picture ? QSize(176, 80) : QSize(232, 44));
    card->setStyleSheet("QFrame#attachmentCard { background: #242527; border: 1px solid #363739; border-radius: 10px; }"
                       "QLabel { border: none; background: transparent; color: #ECECEE; font-size: 12px; padding: 0px; }"
                       "QToolButton { border: none; background: transparent; color: #A5A6AA; font-size: 16px; padding: 0px; }"
                       "QToolButton:hover { color: #ECECEE; background: #363739; border-radius: 5px; }");
    card->setToolTip(attachment.fileName);
    auto* row = new QHBoxLayout(card); row->setContentsMargins(8, 8, 6, 8); row->setSpacing(6);
    const auto imagePath = attachment.previewPath.isEmpty() ? attachment.sourcePath : attachment.previewPath;
    const auto thumbnail = picture ? preview(imagePath, QSize(52, 52)) : QPixmap();
    auto* icon = new QToolButton(card); icon->setFixedSize(picture ? QSize(52, 56) : QSize(24, 26));
    if (!thumbnail.isNull()) {
        icon->setIcon(QIcon(thumbnail)); icon->setIconSize(QSize(52, 52));
        icon->setToolTip(QStringLiteral("Открыть изображение"));
        QObject::connect(icon, &QToolButton::clicked, card, [card, imagePath, attachment]() {
            QDialog dialog(card); dialog.setWindowTitle(attachment.fileName);
            dialog.setStyleSheet("QDialog { background: #191A1B; } QLabel { color: #ECECEE; }");
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
        if (picture) icon->setText(extension.toUpper().left(4));
        else { icon->setIcon(UiStyle::icon(UiStyle::Icon::Documents)); icon->setIconSize(QSize(18, 18)); }
        icon->setEnabled(false);
    }
    row->addWidget(icon);
    auto* details = new QVBoxLayout; details->setSpacing(4);
    auto* name = new QLabel; name->setTextFormat(Qt::PlainText); name->setWordWrap(picture);
    QFont nameFont("Segoe UI"); nameFont.setPixelSize(12); name->setFont(nameFont);
    const int nameWidth = picture ? (remove ? 79 : 100) : (remove ? 152 : 178);
    name->setText(name->fontMetrics().elidedText(attachment.fileName, Qt::ElideMiddle, nameWidth));
    auto* type = new QLabel(picture ? QStringLiteral("Изображение") : QStringLiteral("Документ"));
    type->setStyleSheet("color: #A5A6AA; font-size: 11px;");
    details->addWidget(name);
    if (picture) details->addWidget(type); else delete type;
    row->addLayout(details, 1);
    if (remove) {
        auto* close = new QToolButton(card); close->setText(QString::fromUtf8("×")); close->setFixedSize(18, 22);
        close->setToolTip(QStringLiteral("Убрать вложение"));
        QObject::connect(close, &QToolButton::clicked, card, [remove]() { remove(); });
        row->addWidget(close, 0, Qt::AlignTop);
    }
    return card;
}
