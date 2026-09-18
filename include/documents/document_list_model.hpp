#pragma once

#include "io/document_manager.hpp"

#include <QAbstractListModel>
#include <QVariant>


class DocumentListModel : public QAbstractListModel {
    Q_OBJECT
    DocumentManager* manager_;
public:
    explicit DocumentListModel(DocumentManager* manager, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;

    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
};

