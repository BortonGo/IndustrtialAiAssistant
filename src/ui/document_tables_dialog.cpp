#include "document_tables_dialog.hpp"
#include "source_panel.hpp"
#include "storage/storage_client.hpp"
#include <QAbstractTableModel>
#include <QTableView>
#include <QHeaderView>
#include <QComboBox>
#include <QLabel>
#include <QTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QFileDialog>
#include <QFileInfo>
#include <QSaveFile>
#include <QTimer>

// Lazy cell access avoids constructing hundreds of thousands of widget items.
class StoredTableModel : public QAbstractTableModel {
    QJsonArray rows_;
    int columns_ = 0;
public:
    explicit StoredTableModel(QObject* parent) : QAbstractTableModel(parent) {}
    void setTable(const QJsonObject& table) {
        beginResetModel(); rows_ = table.value("rows").toArray(); columns_ = table.value("columns").toInt(); endResetModel();
    }
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : rows_.size(); }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : columns_; }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() >= rows_.size() || index.column() >= columns_) return {};
        const auto row = rows_[index.row()].toArray();
        const auto text = index.column() < row.size() ? row[index.column()].toString() : QString();
        if (role == Qt::DisplayRole || role == Qt::ToolTipRole) return text.size() > 1000 ? text.left(1000) + QStringLiteral("…") : text;
        if (role == Qt::TextAlignmentRole) return int(Qt::AlignLeft | Qt::AlignTop);
        return {};
    }
    QVariant headerData(int section, Qt::Orientation, int role) const override {
        return role == Qt::DisplayRole ? QVariant(section + 1) : QVariant();
    }
};

bool saveTableExport(const QString& path, const QJsonObject& result, QString* error) {
    const auto encoded = result.value("data").toString().toLatin1();
    const auto bytes = QByteArray::fromBase64(encoded);
    if (bytes.isEmpty() || bytes.toBase64() != encoded) {
        if (error) *error = QStringLiteral("Получен повреждённый файл экспорта.");
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        if (error) *error = QStringLiteral("Не удалось сохранить файл: %1").arg(file.errorString());
        return false;
    }
    return true;
}

DocumentTablesDialog::DocumentTablesDialog(StorageClient* storage, const QString& chatId,
                                         const QString& documentId, QWidget* parent)
    : QDialog(parent), storage_(storage), chatId_(chatId), documentId_(documentId) {
    setAttribute(Qt::WA_DeleteOnClose); setObjectName("documentTables");
    setWindowTitle(QStringLiteral("Таблицы документа")); resize(1020, 720); setMinimumSize(820, 520);
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(22, 20, 22, 20); layout->setSpacing(12);
    auto* heading = new QLabel(QStringLiteral("Таблицы документа")); heading->setStyleSheet("font-size: 20px; font-weight: 600;"); layout->addWidget(heading);
    title_ = new QLabel; title_->setObjectName("tablesDocument"); title_->setTextFormat(Qt::PlainText);
    title_->setWordWrap(true); title_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); layout->addWidget(title_);
    auto* note = new QLabel(QStringLiteral("Выберите таблицу, сверьте с источником и сохраните. Excel сохраняет коды и числа как текст; на отдельном листе — источник. CSV содержит только ячейки, разделитель «;»."));
    note->setWordWrap(true); note->setStyleSheet("color: #A5A6AA;"); layout->addWidget(note);
    tables_ = new QComboBox; tables_->setObjectName("storedTables");
    tables_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); tables_->setMinimumContentsLength(20);
    tables_->setStyleSheet("QComboBox { background: #242527; color: #ECECEE; border: 1px solid #363739; border-radius: 7px; padding: 9px 34px 9px 12px; } QComboBox::drop-down { border: none; width: 28px; } QComboBox::down-arrow { image: url(:/icons/chevron-down.svg); width: 16px; height: 16px; } QComboBox QAbstractItemView { background: #242527; color: #ECECEE; selection-background-color: #343638; border: 1px solid #363739; }");
    layout->addWidget(tables_);
    warnings_ = new QTextEdit; warnings_->setObjectName("tableWarnings"); warnings_->setReadOnly(true); warnings_->setMaximumHeight(80); warnings_->hide();
    warnings_->setStyleSheet("QTextEdit { background: #292723; color: #E8C78D; border: 1px solid #504434; padding: 5px; }"); layout->addWidget(warnings_);
    model_ = new StoredTableModel(this);
    view_ = new QTableView; view_->setObjectName("tableCells"); view_->setModel(model_);
    view_->setEditTriggers(QAbstractItemView::NoEditTriggers); view_->setWordWrap(true);
    view_->horizontalHeader()->setDefaultSectionSize(200); view_->horizontalHeader()->setStretchLastSection(true);
    view_->verticalHeader()->setDefaultSectionSize(48); view_->setAlternatingRowColors(true);
    auto palette = view_->palette();
    palette.setColor(QPalette::Base, QColor("#191A1B")); palette.setColor(QPalette::Window, QColor("#242527"));
    palette.setColor(QPalette::Button, QColor("#242527")); palette.setColor(QPalette::Text, QColor("#ECECEE"));
    view_->setPalette(palette); view_->verticalHeader()->setPalette(palette); view_->horizontalHeader()->setPalette(palette);
    view_->setStyleSheet("QTableView { background: #191A1B; alternate-background-color: #222426; color: #ECECEE; gridline-color: #363739; border: 1px solid #363739; } QTableView::item { padding: 8px; } QTableView::item:selected { background: #343638; color: #FFFFFF; } QHeaderView { background: #242527; } QHeaderView::section, QTableCornerButton::section { background: #242527; color: #A5A6AA; border: none; padding: 8px; }");
    layout->addWidget(view_, 1);
    auto* hint = new QLabel(QStringLiteral("Порядок строк и пустые ячейки сохранены. Длинные ячейки сокращены только в предпросмотре. Для кодов с ведущими нулями используйте Excel."));
    hint->setWordWrap(true); hint->setStyleSheet("color: #A5A6AA; font-size: 12px;"); layout->addWidget(hint);
    status_ = new QLabel; status_->setObjectName("tableStatus"); status_->setTextFormat(Qt::PlainText); status_->setWordWrap(true);
    status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); layout->addWidget(status_);
    auto* actions = new QHBoxLayout;
    source_ = new QPushButton(QStringLiteral("Источник")); source_->setObjectName("tableSource");
    refresh_ = new QPushButton(QStringLiteral("Обновить")); refresh_->setObjectName("refreshTables");
    csv_ = new QPushButton(QStringLiteral("Сохранить CSV")); csv_->setObjectName("exportTableCsv");
    xlsx_ = new QPushButton(QStringLiteral("Сохранить Excel")); xlsx_->setObjectName("exportTableXlsx");
    auto* close = new QPushButton(QStringLiteral("Закрыть"));
    actions->addWidget(source_); actions->addWidget(refresh_); actions->addStretch(); actions->addWidget(csv_); actions->addWidget(xlsx_); actions->addWidget(close);
    layout->addLayout(actions);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(refresh_, &QPushButton::clicked, this, [this]() { load(); });
    connect(tables_, static_cast<void(QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this, [this]() { selectTable(); });
    connect(source_, &QPushButton::clicked, this, [this]() {
        if (preview_) preview_->close();
        preview_ = showSourcePreview(selectedTable().value("reference").toObject(), this);
    });
    connect(csv_, &QPushButton::clicked, this, [this]() { exportSelected("csv"); });
    connect(xlsx_, &QPushButton::clicked, this, [this]() { exportSelected("xlsx"); });
    updateActions(); QTimer::singleShot(0, this, [this]() { load(); });
}

QJsonObject DocumentTablesDialog::selectedTable() const {
    const auto values = report_.value("tables").toArray(); const int index = tables_->currentIndex();
    return index >= 0 && index < values.size() ? values[index].toObject() : QJsonObject();
}

void DocumentTablesDialog::updateActions() {
    const bool ready = !busy_ && !selectedTable().isEmpty();
    source_->setEnabled(ready); csv_->setEnabled(ready); xlsx_->setEnabled(ready);
    tables_->setEnabled(!busy_); refresh_->setEnabled(!busy_);
}

void DocumentTablesDialog::selectTable() {
    if (preview_) preview_->close();
    model_->setTable(selectedTable()); updateActions();
}

void DocumentTablesDialog::load() {
    if (busy_ || !storage_) return;
    busy_ = true; report_ = {}; tables_->clear(); selectTable();
    warnings_->hide(); status_->setText(QStringLiteral("Загружаю сохранённые таблицы…"));
    QPointer<DocumentTablesDialog> guard(this);
    storage_->request("document_tables", {{"chatId", chatId_}, {"documentId", documentId_}},
        [guard](const QJsonObject& result, const QString& error) {
            if (!guard) return;
            guard->busy_ = false;
            if (!error.isEmpty()) { guard->status_->setText(error); guard->updateActions(); return; }
            guard->report_ = result;
            const auto path = result.value("sourcePath").toString();
            guard->title_->setText(QFileInfo(path).fileName()); guard->title_->setToolTip(path);
            QStringList warnings;
            for (const auto& value : result.value("warnings").toArray()) warnings.append(value.toString());
            guard->warnings_->setPlainText(warnings.join("\n")); guard->warnings_->setVisible(!warnings.isEmpty());
            const auto tables = result.value("tables").toArray();
            for (const auto& value : tables) {
                const auto table = value.toObject();
                const auto origin = table.value("origin").toString();
                const auto kind = origin == "native" ? QStringLiteral("Исходная таблица") : origin == "recognition" ? QStringLiteral("Распознано с изображения") : QStringLiteral("Текстовая разметка");
                const int page = table.value("reference").toObject().value("pageFirst").toInt();
                guard->tables_->addItem(QStringLiteral("Таблица %1 · %2 × %3 · %4%5").arg(table.value("number").toInt())
                    .arg(table.value("rows").toArray().size()).arg(table.value("columns").toInt()).arg(kind)
                    .arg(page ? QStringLiteral(" · стр. %1").arg(page) : QString()));
            }
            guard->status_->setText(tables.isEmpty() ? QStringLiteral("Готовых таблиц с выделенными ячейками нет. В PDF таблица могла сохраниться обычным текстом; автоматическое восстановление столбцов здесь не выполняется.") :
                QStringLiteral("Таблиц: %1%2").arg(tables.size()).arg(result.value("partial").toBool() ? QStringLiteral(" · Данные документа неполные") : QString()));
            guard->selectTable();
        });
}

void DocumentTablesDialog::exportSelected(const QString& format) {
    if (busy_ || !storage_ || selectedTable().isEmpty()) return;
    const auto table = selectedTable();
    const auto base = QFileInfo(report_.value("sourcePath").toString()).completeBaseName();
    auto* chooser = new QFileDialog(this, QStringLiteral("Сохранить таблицу"), base + QString("-table-%1.%2").arg(table.value("number").toInt()).arg(format),
                        format == "xlsx" ? QStringLiteral("Excel (*.xlsx)") : QStringLiteral("CSV UTF-8 (*.csv)"));
    chooser->setAttribute(Qt::WA_DeleteOnClose);
    chooser->setAcceptMode(QFileDialog::AcceptSave); chooser->setFileMode(QFileDialog::AnyFile); chooser->setDefaultSuffix(format);
    const auto snapshot = report_.value("snapshot");
    connect(chooser, &QFileDialog::fileSelected, this, [this, format, table, snapshot](const QString& path) {
        if (busy_ || !storage_) return;
        if (QFileInfo(path).suffix().toLower() != format) { status_->setText(QStringLiteral("Выберите файл с расширением .%1").arg(format)); return; }
        busy_ = true; updateActions(); status_->setText(QStringLiteral("Подготавливаю файл…"));
        QPointer<DocumentTablesDialog> guard(this);
        storage_->request("export_document_table", {{"chatId", chatId_}, {"documentId", documentId_},
            {"snapshot", snapshot}, {"tableId", table.value("id")}, {"format", format}},
            [guard, path](const QJsonObject& result, const QString& error) {
                if (!guard) return;
                guard->busy_ = false; guard->updateActions();
                if (!error.isEmpty()) { guard->status_->setText(error); return; }
                QString failure;
                if (!saveTableExport(path, result, &failure)) { guard->status_->setText(failure); return; }
                auto text = QStringLiteral("Сохранено: %1").arg(path);
                if (result.value("protectedCells").toInt()) text += QStringLiteral("\nВ %1 ячейках CSV добавлен апостроф, чтобы текст не выполнялся как формула.").arg(result.value("protectedCells").toInt());
                guard->status_->setText(text);
            });
    });
    chooser->open();
}
