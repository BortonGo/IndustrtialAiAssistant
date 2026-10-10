#include "document_comparison_dialog.hpp"
#include "source_panel.hpp"
#include "storage/storage_client.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QTextEdit>
#include <QTableWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QFileInfo>
#include <QJsonArray>
#include <QTimer>

namespace {
QString location(const QJsonObject& source) {
    QStringList result;
    const int page = source.value("pageFirst").toInt();
    if (page > 0) result.append(QStringLiteral("Стр. %1").arg(page));
    for (const auto& index : source.value("tableIndices").toArray())
        result.append(QStringLiteral("Таблица %1").arg(index.toInt() + 1));
    if (!source.value("section").toString().isEmpty()) result.append(source.value("section").toString());
    return result.join(QStringLiteral(" · "));
}
QString excerpt(const QJsonObject& source) {
    if (source.isEmpty()) return QStringLiteral("—");
    const auto text = source.value(source.contains("displayText") ? "displayText" : "text").toString();
    return text.size() > 240 ? text.left(240) + QStringLiteral("…") : text;
}
}

DocumentComparisonDialog::DocumentComparisonDialog(StorageClient* storage, const QString& chatId,
                                                   const QStringList& ids, QWidget* parent) : QDialog(parent) {
    setObjectName("documentComparison"); setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("Сравнение документов")); resize(1060, 700); setMinimumSize(860, 500);
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(22, 20, 22, 20); layout->setSpacing(12);
    auto* heading = new QLabel(QStringLiteral("Сравнение документов"));
    heading->setStyleSheet("font-size: 20px; font-weight: 600;"); layout->addWidget(heading);
    auto* note = new QLabel(QStringLiteral("Сравниваются сохранённые строки текста и таблиц. Пробелы внутри строк не учитываются; переносы, числа и регистр учитываются. Это перечень изменений, а не оценка их смысла."));
    note->setWordWrap(true); note->setStyleSheet("color: #A5A6AA;"); layout->addWidget(note);
    files_ = new QLabel; files_->setObjectName("comparisonFiles"); files_->setTextFormat(Qt::PlainText);
    files_->setWordWrap(true); files_->setMaximumHeight(80);
    files_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); layout->addWidget(files_);
    summary_ = new QLabel(QStringLiteral("Сравниваю сохранённые документы…"));
    summary_->setObjectName("comparisonSummary"); summary_->setTextFormat(Qt::PlainText); summary_->setWordWrap(true);
    layout->addWidget(summary_);
    warnings_ = new QTextEdit; warnings_->setObjectName("comparisonWarnings"); warnings_->setReadOnly(true);
    warnings_->setMaximumHeight(85); warnings_->hide();
    warnings_->setStyleSheet("QTextEdit { background: #292723; color: #E8C78D; border: 1px solid #504434; border-radius: 6px; padding: 5px; }");
    layout->addWidget(warnings_);
    table_ = new QTableWidget(0, 3); table_->setObjectName("comparisonChanges");
    table_->setHorizontalHeaderLabels({QStringLiteral("Изменение / место"), QStringLiteral("Было"), QStringLiteral("Стало")});
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed); table_->setColumnWidth(0, 190);
    table_->verticalHeader()->hide(); table_->verticalHeader()->setDefaultSectionSize(100);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows); table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setWordWrap(true); table_->setShowGrid(false);
    table_->setStyleSheet("QTableWidget { background: #191A1B; color: #ECECEE; border: 1px solid #363739; border-radius: 8px; } QTableWidget::item { padding: 8px; border-bottom: 1px solid #363739; } QTableWidget::item:selected { background: #343638; color: #FFFFFF; } QHeaderView::section { background: #242527; color: #A5A6AA; border: none; padding: 10px; }");
    layout->addWidget(table_, 1);
    auto* hint = new QLabel(QStringLiteral("Выберите изменение и откройте полный фрагмент. Номера таблиц указаны по порядку в файле."));
    hint->setWordWrap(true); hint->setStyleSheet("color: #A5A6AA; font-size: 12px;"); layout->addWidget(hint);
    auto* actions = new QHBoxLayout;
    before_ = new QPushButton(QStringLiteral("Открыть «Было»")); before_->setObjectName("comparisonBefore");
    after_ = new QPushButton(QStringLiteral("Открыть «Стало»")); after_->setObjectName("comparisonAfter");
    swap_ = new QPushButton(QStringLiteral("Поменять местами")); swap_->setObjectName("comparisonSwap"); swap_->setEnabled(false);
    actions->addWidget(before_); actions->addWidget(after_); actions->addWidget(swap_); actions->addStretch();
    auto* close = new QPushButton(QStringLiteral("Закрыть")); actions->addWidget(close); layout->addLayout(actions);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(table_, &QTableWidget::itemSelectionChanged, this, [this]() { updateSelection(); });
    connect(before_, &QPushButton::clicked, this, [this]() { openSource(false); });
    connect(after_, &QPushButton::clicked, this, [this]() { openSource(true); });
    connect(table_, &QTableWidget::cellDoubleClicked, this, [this]() { openSource(selectedSource(false).isEmpty()); });
    connect(swap_, &QPushButton::clicked, this, [this]() { reversed_ = !reversed_; displayReport(); });
    updateSelection();
    // The window may be closed or the chat switched while storage is answering.
    QPointer<DocumentComparisonDialog> guard(this);
    QPointer<StorageClient> storageGuard(storage);
    QTimer::singleShot(0, this, [guard, storageGuard, chatId, ids]() {
        if (!guard || !storageGuard) return;
        storageGuard->request("compare_documents", {{"chatId", chatId}, {"documentIds", QJsonArray::fromStringList(ids)}},
            [guard](const QJsonObject& report, const QString& error) {
                if (!guard) return;
                if (!error.isEmpty()) {
                    guard->summary_->setText(QStringLiteral("Не удалось сравнить документы: %1").arg(error)); return;
                }
                guard->report_ = report; guard->swap_->setEnabled(true); guard->displayReport();
            });
    });
}

void DocumentComparisonDialog::displayReport() {
    if (preview_) preview_->close();
    const auto left = report_.value(reversed_ ? "right" : "left").toObject().value("sourcePath").toString();
    const auto right = report_.value(reversed_ ? "left" : "right").toObject().value("sourcePath").toString();
    files_->setText(QStringLiteral("Было: %1\nСтало: %2").arg(QFileInfo(left).fileName(), QFileInfo(right).fileName()));
    files_->setToolTip(QStringLiteral("Было: %1\nСтало: %2").arg(left, right));
    const auto counts = report_.value("counts").toObject();
    const int added = counts.value(reversed_ ? "removed" : "added").toInt();
    const int removed = counts.value(reversed_ ? "added" : "removed").toInt();
    const int changed = counts.value("changed").toInt();
    const bool partial = report_.value("partial").toBool();
    summary_->setText(added + removed + changed ?
        QStringLiteral("Изменено: %1 · Добавлено: %2 · Удалено: %3%4").arg(changed).arg(added).arg(removed)
            .arg(partial ? QStringLiteral(" · Неполное сравнение") : QString()) :
        (partial ? QStringLiteral("В доступных данных различий нет. Обработка документов неполная.") :
                   QStringLiteral("Различий в извлечённых данных не найдено.")));
    QStringList warnings;
    // Keep warnings tied to the original file names when the direction changes.
    for (const auto& value : report_.value("warnings").toArray()) warnings.append(value.toString());
    warnings_->setPlainText(warnings.join("\n")); warnings_->setVisible(!warnings.isEmpty());
    const auto changes = report_.value("changes").toArray();
    table_->setRowCount(0); table_->setRowCount(changes.size());
    for (int row = 0; row < changes.size(); ++row) {
        const auto change = changes[row].toObject();
        auto kind = change.value("kind").toString();
        if (reversed_ && kind != "changed") kind = kind == "added" ? "removed" : "added";
        const auto old = change.value(reversed_ ? "after" : "before").toObject();
        const auto next = change.value(reversed_ ? "before" : "after").toObject();
        const auto label = kind == "added" ? QStringLiteral("Добавлено") : kind == "removed" ? QStringLiteral("Удалено") : QStringLiteral("Изменено");
        auto* status = new QTableWidgetItem(label + "\n" + location(old.isEmpty() ? next : old));
        status->setForeground(QColor(kind == "added" ? "#8ED4AE" : kind == "removed" ? "#EFA2A2" : "#E8C78D"));
        table_->setItem(row, 0, status);
        table_->setItem(row, 1, new QTableWidgetItem(excerpt(old)));
        table_->setItem(row, 2, new QTableWidgetItem(excerpt(next)));
        for (int column = 0; column < 3; ++column) table_->item(row, column)->setTextAlignment(Qt::AlignLeft | Qt::AlignTop);
    }
    if (!changes.isEmpty()) table_->selectRow(0);
    updateSelection();
}

QJsonObject DocumentComparisonDialog::selectedSource(bool after) const {
    const auto changes = report_.value("changes").toArray();
    const int row = table_->currentRow();
    if (row < 0 || row >= changes.size()) return {};
    return changes[row].toObject().value(after != reversed_ ? "after" : "before").toObject();
}

void DocumentComparisonDialog::updateSelection() {
    before_->setEnabled(!selectedSource(false).isEmpty()); after_->setEnabled(!selectedSource(true).isEmpty());
}

void DocumentComparisonDialog::openSource(bool after) {
    const auto source = selectedSource(after);
    if (source.isEmpty()) return;
    if (preview_) preview_->close();
    preview_ = showSourcePreview(source, this);
}
