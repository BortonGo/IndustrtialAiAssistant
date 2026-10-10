#include "documents_widget.hpp"
#include "ui_style.hpp"
#include "documents/document_list_model.hpp"

#include <QVBoxLayout>
#include <QPushButton>
#include <QListView>
#include <QLabel>
#include <QToolButton>
#include <QHBoxLayout>
#include <QListWidget>
#include <QJsonObject>
#include <QFileInfo>

DocumentsWidget::DocumentsWidget(QAbstractItemModel* model, QWidget* parent) : QWidget(parent){
    auto* pageLayout = new QVBoxLayout(this);
    pageLayout->setContentsMargins(0, 0, 0, 0); pageLayout->setSpacing(0);
    auto* toolbar = new QWidget;
    toolbar->setObjectName("workspaceHeader"); toolbar->setAttribute(Qt::WA_StyledBackground, true);
    toolbar->setFixedHeight(58);
    auto* header = new QHBoxLayout(toolbar); header->setContentsMargins(18, 10, 18, 10);
    auto* sidebarButton = new QToolButton;
    sidebarButton->setIcon(UiStyle::icon(UiStyle::Icon::Sidebar)); sidebarButton->setFixedSize(32, 32);
    sidebarButton->setToolTip(QStringLiteral("Показать или скрыть чаты"));
    sidebarButton->setAccessibleName(sidebarButton->toolTip());
    header->addWidget(sidebarButton);
    auto* title = new QLabel(QStringLiteral("Документы чата"));
    title->setStyleSheet("font-weight: 600; padding-left: 8px;");
    header->addWidget(title, 1);
    auto* back = new QPushButton(QStringLiteral("К переписке"));
    back->setIcon(UiStyle::icon(UiStyle::Icon::Back));
    header->addWidget(back); pageLayout->addWidget(toolbar);
    connect(sidebarButton, &QToolButton::clicked, this, &DocumentsWidget::sidebarToggleRequested);
    connect(back, &QPushButton::clicked, this, &DocumentsWidget::chatPageRequested);
    auto* documentsLayout = new QVBoxLayout;
    documentsLayout->setContentsMargins(30, 24, 30, 20); documentsLayout->setSpacing(16);
    pageLayout->addLayout(documentsLayout, 1);

    auto* documentsLabel = new QLabel("Документы и изображения текущего чата");
    documentsLabel->setStyleSheet(
        "color: #A5A6AA;"
        " font-size: 14px;"
        " padding: 6px;"
    );
    documentsLayout->addWidget(documentsLabel);
    auto* btnAddDocument = new QPushButton("Добавить файл");
    btnAddDocument->setIcon(UiStyle::icon(UiStyle::Icon::Plus));
    btnAddDocument->setStyleSheet("QPushButton { border: 1px solid #363739; padding: 8px 12px; }");
    uploadButton_ = btnAddDocument;
    auto* documentActions = new QHBoxLayout;
    compare_ = new QPushButton(QStringLiteral("Сравнить два документа"));
    compare_->setObjectName("compareDocuments");
    compare_->setToolTip(QStringLiteral("Выберите два обработанных документа с зажатым Ctrl"));
    documentActions->addWidget(btnAddDocument); documentActions->addWidget(compare_); documentActions->addStretch();
    documentsLayout->addLayout(documentActions);

    importsLabel_ = new QLabel(QStringLiteral("Очередь обработки · можно задавать вопросы во время импорта"));
    importsLabel_->setWordWrap(true);
    documentsLayout->addWidget(importsLabel_);
    importsList_ = new QListWidget;
    importsList_->setObjectName("documentImportQueue");
    importsList_->setAccessibleName(QStringLiteral("Очередь обработки документов"));
    importsList_->setStyleSheet("QListWidget { background: #242527; border: 1px solid #363739; border-radius: 8px; } QListWidget::item { padding: 8px; } QListWidget::item:selected { background: #323437; }");
    documentsLayout->addWidget(importsList_);
    auto* queueActions = new QHBoxLayout;
    pauseImport_ = new QPushButton(QStringLiteral("Пауза")); pauseImport_->setObjectName("pauseImport");
    resumeImport_ = new QPushButton(QStringLiteral("Продолжить")); resumeImport_->setObjectName("resumeImport");
    cancelImport_ = new QPushButton(QStringLiteral("Убрать из очереди")); cancelImport_->setObjectName("cancelImport");
    for (auto* button : {pauseImport_, resumeImport_, cancelImport_})
        button->setStyleSheet("QPushButton { border: 1px solid #363739; padding: 6px 12px; }");
    queueActions->addWidget(pauseImport_); queueActions->addWidget(resumeImport_); queueActions->addWidget(cancelImport_); queueActions->addStretch();
    documentsLayout->addLayout(queueActions);
    connect(importsList_, &QListWidget::currentRowChanged, this, [this]() { updateImportActions(); });
    auto action = [this](const QString& status) {
        if (auto* item = importsList_->currentItem()) emit importActionRequested(item->data(Qt::UserRole).toString(), status);
    };
    connect(pauseImport_, &QPushButton::clicked, this, [action]() { action("paused"); });
    connect(resumeImport_, &QPushButton::clicked, this, [action]() { action("queued"); });
    connect(cancelImport_, &QPushButton::clicked, this, [action]() { action("cancelled"); });
    setImportJobs({}, {});

    QListView* documentListView = new QListView(this);
    documents_ = documentListView;
    documents_->setObjectName("chatDocuments");
    documents_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    documentListView->setModel(model);
    documentListView->setStyleSheet(
        "QListView {"
        " background-color: #191A1B;"
        " color: #ECECEE;"
        " border: none;"
        " font-size: 14px;"
        " outline: none;"
        "}"
        "QListView::item {"
        " padding: 12px;"
        " margin-bottom: 4px;"
        " border-radius: 10px;"
        "}"
        "QListView::item:hover {"
        " background-color: #242527;"
        "}"
        "QListView::item:selected {"
        " background-color: #323437;"
        " color: #ECECEE;"
        "}"
    );
    documentsLayout->addWidget(documentListView, 2);
    auto* selectionHint = new QLabel(QStringLiteral("Для сравнения выберите два документа с зажатым Ctrl. Направление сравнения можно поменять в отчёте."));
    selectionHint->setWordWrap(true); selectionHint->setStyleSheet("color: #A5A6AA; font-size: 12px;");
    documentsLayout->addWidget(selectionHint);
    connect(documents_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]() { updateComparisonAction(); });
    connect(model, &QAbstractItemModel::modelReset, this, [this]() { updateComparisonAction(); });
    connect(model, &QAbstractItemModel::dataChanged, this, [this]() { updateComparisonAction(); });
    connect(model, &QAbstractItemModel::rowsRemoved, this, [this]() { updateComparisonAction(); });
    connect(compare_, &QPushButton::clicked, this, [this]() {
        QStringList ids;
        // Stable list order, independent of the order of Ctrl-clicks.
        for (int row = 0; row < documents_->model()->rowCount(); ++row) {
            const auto index = documents_->model()->index(row, 0);
            if (documents_->selectionModel()->isSelected(index)) ids.append(index.data(Qt::UserRole).toString());
        }
        if (ids.size() == 2 && compare_->isEnabled()) emit compareRequested(ids);
    });
    updateComparisonAction();

    connect(btnAddDocument, &QPushButton::clicked,
            this, &DocumentsWidget::documentUploadRequested);
}

void DocumentsWidget::setUploadEnabled(bool enabled) { uploadButton_->setEnabled(enabled); }

void DocumentsWidget::setComparisonEnabled(bool enabled) {
    comparisonEnabled_ = enabled; updateComparisonAction();
}

void DocumentsWidget::updateComparisonAction() {
    const auto selected = documents_->selectionModel()->selectedRows();
    bool ready = comparisonEnabled_ && selected.size() == 2;
    for (const auto& index : selected) ready = ready && index.data(DocumentListModel::ComparisonReadyRole).toBool();
    compare_->setEnabled(ready);
}

void DocumentsWidget::setImportJobs(const QJsonArray& jobs, const QString& chatId) {
    const auto selected = importsList_->currentItem() ? importsList_->currentItem()->data(Qt::UserRole).toString() : QString();
    importsList_->blockSignals(true); importsList_->clear();
    for (const auto& value : jobs) {
        const auto job = value.toObject();
        if (job.value("chatId").toString() != chatId) continue;
        const auto status = job.value("status").toString();
        const auto label = status == "queued" ? QStringLiteral("В очереди") : status == "paused" ? QStringLiteral("Пауза") :
                           status == "failed" ? QStringLiteral("Ошибка") : QStringLiteral("Обработка");
        auto* item = new QListWidgetItem(QFileInfo(job.value("sourcePath").toString()).fileName() + " · " + label + "\n" +
            (job.value("error").toString().isEmpty() ? job.value("progress").toString() : job.value("error").toString()), importsList_);
        item->setData(Qt::UserRole, job.value("id").toString()); item->setData(Qt::UserRole + 1, status);
        item->setToolTip(job.value("sourcePath").toString() + "\n" + item->text());
        if (job.value("id").toString() == selected) importsList_->setCurrentItem(item);
    }
    if (!importsList_->currentItem() && importsList_->count()) importsList_->setCurrentRow(0);
    importsList_->blockSignals(false);
    importsList_->setFixedHeight(qBound(66, importsList_->count() * 58 + 8, 190));
    const bool visible = importsList_->count() > 0;
    importsLabel_->setVisible(visible); importsList_->setVisible(visible);
    pauseImport_->setVisible(visible); resumeImport_->setVisible(visible); cancelImport_->setVisible(visible);
    updateImportActions();
}

void DocumentsWidget::updateImportActions() {
    const auto* item = importsList_->currentItem();
    const auto status = item ? item->data(Qt::UserRole + 1).toString() : QString();
    pauseImport_->setEnabled(status == "queued" || status == "running");
    resumeImport_->setEnabled(status == "paused" || status == "failed");
    cancelImport_->setEnabled(item != nullptr);
}
