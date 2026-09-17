#include "documents_widget.hpp"

#include <QVBoxLayout>
#include <QPushButton>
#include <QListView>
#include <QLabel>

DocumentsWidget::DocumentsWidget(QAbstractItemModel* model, QWidget* parent) : QWidget(parent){
    auto* documentsLayout = new QVBoxLayout(this);
    documentsLayout->setContentsMargins(16, 16, 16, 16);
    documentsLayout->setSpacing(16);

    auto* documentsLabel = new QLabel("Документы · общие");
    documentsLabel->setStyleSheet(
        "color: #B8BCC6;"
        " font-size: 14px;"
        " padding: 6px;"
    );
    documentsLayout->addWidget(documentsLabel);
    auto* btnAddDocument = new QPushButton("Добавить документ");
    documentsLayout->addWidget(btnAddDocument, 0, Qt::AlignLeft);

    QListView* documentListView = new QListView(this);
    documentListView->setModel(model);
    documentListView->setStyleSheet(
        "QListView {"
        " background-color: #18191C;"
        " color: #E8E8ED;"
        " border: none;"
        " font-size: 16px;"
        " outline: none;"
        "}"
        "QListView::item {"
        " padding: 12px;"
        " margin-bottom: 4px;"
        " border-radius: 10px;"
        "}"
        "QListView::item:hover {"
        " background-color: #24262B;"
        "}"
        "QListView::item:selected {"
        " background-color: #353C50;"
        " color: #FFFFFF;"
        "}"
    );
    documentsLayout->addWidget(documentListView, 2);

    connect(btnAddDocument, &QPushButton::clicked,
            this, &DocumentsWidget::documentUploadRequested);
}
