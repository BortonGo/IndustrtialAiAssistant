#include "source_panel.hpp"
#include "markdown_render.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTextBrowser>
#include <QTextDocument>
#include <QJsonObject>
#include <QFileInfo>
#include <QDir>
#include <QDialog>
#include <QImageReader>
#include <QDesktopServices>
#include <QUrl>

namespace {
QString position(const QJsonObject& source) {
    QStringList parts;
    const int first = source.value("pageFirst").toInt(), last = source.value("pageLast").toInt();
    if (first > 0) parts.append(last > first ? QStringLiteral("Страницы %1–%2").arg(first).arg(last) : QStringLiteral("Страница %1").arg(first));
    else parts.append(QStringLiteral("Страница не определена"));
    QStringList tables;
    for (const auto& index : source.value("tableIndices").toArray()) tables.append(QString::number(index.toInt() + 1));
    if (!tables.isEmpty()) parts.append(QStringLiteral("Таблицы по порядку в файле: %1").arg(tables.join(", ")));
    if (!source.value("imagePath").toString().isEmpty()) parts.append(QStringLiteral("Изображение"));
    if (!source.value("section").toString().isEmpty()) parts.append(source.value("section").toString());
    return parts.join(QStringLiteral(" · "));
}
class SnapshotDocument : public QTextDocument {
protected:
    // Markdown in documents is untrusted: never load linked local/network images.
    QVariant loadResource(int, const QUrl&) override { return {}; }
public:
    explicit SnapshotDocument(QObject* parent) : QTextDocument(parent) {}
};
class SnapshotBrowser : public QTextBrowser {
public:
    explicit SnapshotBrowser(QWidget* parent) : QTextBrowser(parent) {
        setDocument(new SnapshotDocument(this));
        setOpenLinks(false); setOpenExternalLinks(false);
        setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    }
};
bool canOpenOriginal(const QString& path) {
    const auto normalized = QDir::fromNativeSeparators(path);
    const QFileInfo file(path);
    const QStringList extensions{"pdf", "docx", "doc", "txt", "png", "jpg", "jpeg", "webp", "bmp"};
    return !normalized.startsWith("//") && !normalized.contains("://") && file.isAbsolute() &&
           !file.isSymLink() && extensions.contains(file.suffix().toLower()) && file.isFile();
}
}

SourcePanel::SourcePanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(12);
    question_ = new QLabel; question_->setObjectName("sourceQuestion"); question_->setWordWrap(true);
    question_->setTextFormat(Qt::PlainText); question_->setMaximumHeight(90);
    layout->addWidget(question_);
    auto* note = new QLabel(QStringLiteral("Материалы, переданные модели для этого ответа. Откройте фрагмент, чтобы сверить факты."));
    note->setWordWrap(true); note->setStyleSheet("color: #A5A6AA; font-size: 12px;"); layout->addWidget(note);
    list_ = new QListWidget; list_->setObjectName("answerSources"); list_->setWordWrap(true);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setStyleSheet("QListWidget { background: transparent; border: none; } QListWidget::item { padding: 12px 6px; border-bottom: 1px solid #363739; } QListWidget::item:selected { background: #343638; border-radius: 8px; }");
    layout->addWidget(list_, 1);
    open_ = new QPushButton(QStringLiteral("Открыть фрагмент")); open_->setObjectName("openSourceFragment");
    layout->addWidget(open_);
    legacy_ = new QTextEdit; legacy_->setObjectName("sourceText"); legacy_->setReadOnly(true);
    legacy_->setPlaceholderText(QStringLiteral("Для этого ответа источники не использовались или ещё не сохранены."));
    legacy_->setStyleSheet("background: transparent; border: none; color: #A5A6AA; font-size: 13px;");
    layout->addWidget(legacy_, 1);
    connect(open_, &QPushButton::clicked, this, [this]() { openSource(list_->currentRow()); });
    connect(list_, &QListWidget::itemActivated, this, [this]() { openSource(list_->currentRow()); });
    connect(list_, &QListWidget::itemClicked, this, [this]() { openSource(list_->currentRow()); });
    setSources({}, {}, {});
}

void SourcePanel::setSources(const QString& question, const QString& snapshot, const QJsonArray& references) {
    if (preview_) preview_->close();
    references_ = references;
    question_->setText(question); question_->setToolTip(question); question_->setVisible(!question.isEmpty());
    list_->clear();
    for (const auto& value : references) {
        const auto source = value.toObject();
        auto* item = new QListWidgetItem(QStringLiteral("[%1] %2\n%3").arg(source.value("number").toInt())
            .arg(QFileInfo(source.value("sourcePath").toString()).fileName(), position(source)), list_);
        item->setToolTip(source.value("sourcePath").toString());
    }
    list_->setVisible(!references.isEmpty()); open_->setVisible(!references.isEmpty());
    if (!references.isEmpty()) list_->setCurrentRow(0);
    legacy_->setPlainText(snapshot); legacy_->setVisible(references.isEmpty());
}

void SourcePanel::openSource(int index) {
    if (index < 0 || index >= references_.size()) return;
    if (preview_) preview_->close();
    const auto source = references_[index].toObject();
    const auto path = source.value("sourcePath").toString();
    auto* dialog = new QDialog(this); preview_ = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->setObjectName("sourcePreview");
    dialog->setWindowTitle(QStringLiteral("Источник [%1] — %2").arg(source.value("number").toInt()).arg(QFileInfo(path).fileName()));
    dialog->resize(860, 660); dialog->setMinimumSize(520, 360);
    auto* layout = new QVBoxLayout(dialog); layout->setContentsMargins(20, 20, 20, 20); layout->setSpacing(12);
    auto* heading = new QLabel(position(source)); heading->setObjectName("sourcePosition");
    heading->setWordWrap(true); heading->setTextFormat(Qt::PlainText); layout->addWidget(heading);
    auto* note = new QLabel(QStringLiteral("Сохранённый фрагмент на момент ответа. Исходный файл мог измениться."));
    note->setWordWrap(true); note->setStyleSheet("color: #A5A6AA;"); layout->addWidget(note);
    const auto imagePath = source.value("imagePath").toString();
    if (!imagePath.isEmpty()) {
        auto* picture = new QLabel; picture->setObjectName("sourceImage"); picture->setAlignment(Qt::AlignCenter);
        QImageReader reader(imagePath); reader.setAutoTransform(true);
        if (reader.size().isValid()) reader.setScaledSize(reader.size().scaled(780, 260, Qt::KeepAspectRatio));
        const auto image = reader.read();
        if (image.isNull()) picture->setText(QStringLiteral("Сохранённое изображение недоступно. Текст фрагмента показан ниже."));
        else picture->setPixmap(QPixmap::fromImage(image).scaled(780, 260, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        layout->addWidget(picture);
    }
    auto* text = new SnapshotBrowser(dialog); text->setObjectName("sourceExcerpt");
    text->setStyleSheet("QTextBrowser { background: #191A1B; color: #ECECEE; border: 1px solid #363739; border-radius: 8px; padding: 12px; font-size: 14px; }");
    text->document()->setDefaultStyleSheet("body { color: #ECECEE; } td, th { padding: 6px; } th { text-align: left; } p { margin-bottom: 10px; }");
    auto html = markdownToHtml(source.value("text").toString());
    html.replace("<table>", "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"7\">");
    html.replace("<th>", "<th align=\"left\" bgcolor=\"#292B2D\">");
    text->setHtml(html); layout->addWidget(text, 1);
    auto* status = new QLabel; status->setTextFormat(Qt::PlainText); status->setWordWrap(true); layout->addWidget(status);
    auto* actions = new QHBoxLayout;
    auto* original = new QPushButton(QStringLiteral("Открыть исходный файл")); original->setObjectName("openSourceOriginal");
    original->setToolTip(path); original->setEnabled(canOpenOriginal(path));
    if (!original->isEnabled()) status->setText(QStringLiteral("Исходный файл недоступен. Сохранённый фрагмент остаётся доступным."));
    actions->addWidget(original); actions->addStretch();
    auto* close = new QPushButton(QStringLiteral("Закрыть")); actions->addWidget(close); layout->addLayout(actions);
    connect(close, &QPushButton::clicked, dialog, &QDialog::close);
    connect(original, &QPushButton::clicked, dialog, [path, status]() {
        if (!canOpenOriginal(path) || !QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
            status->setText(QStringLiteral("Не удалось открыть исходный файл."));
    });
    dialog->show();
}
