#include "document_list_model.hpp"

#include <QFileInfo>

DocumentListModel::DocumentListModel(DocumentManager* manager, QObject* parent)
    : QAbstractListModel(parent), manager_(manager) {

    connect(manager_, &DocumentManager::documentAboutToBeAdded,
            this, [this](int row) {
        beginInsertRows(QModelIndex(), row, row);
    });

    connect(manager_, &DocumentManager::documentLoaded,
            this, [this]() {
        endInsertRows();
    });

    connect(manager_, &DocumentManager::documentStatusChanged,
            this, [this](const QString& documentId) {
        for (int row = 0, sz = rowCount(); row < sz; ++row) {
            auto* document = manager_->documentAt(row);
            if (document->id == documentId) {
                const QModelIndex idx = index(row, 0);
                emit dataChanged(idx, idx, {Qt::DisplayRole});
                break;
            }
        }
    });

}

int DocumentListModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return manager_->documentCount();
}

QVariant DocumentListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.column() != 0) {
        return {};
    }

    auto* document = manager_->documentAt(index.row());

    if (!document) {
        return {};
    }
    if (role == Qt::DisplayRole) {
        return QFileInfo(document->sourcePath).fileName() + " · " + Document::statusToQString(document->status);
    } else if (role == Qt::UserRole) {
        return document->id;
    } else if (role == Qt::ToolTipRole) {
        return document->sourcePath;
    }
    return {};
}
