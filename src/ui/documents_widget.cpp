#include "documents_widget.hpp"
#include "ui_style.hpp"

#include <QVBoxLayout>
#include <QPushButton>
#include <QListView>
#include <QLabel>
#include <QToolButton>
#include <QHBoxLayout>

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
    documentsLayout->addWidget(btnAddDocument, 0, Qt::AlignLeft);

    QListView* documentListView = new QListView(this);
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

    connect(btnAddDocument, &QPushButton::clicked,
            this, &DocumentsWidget::documentUploadRequested);
}

void DocumentsWidget::setUploadEnabled(bool enabled) { uploadButton_->setEnabled(enabled); }
