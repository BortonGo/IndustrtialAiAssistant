#include "chat_list_model.hpp"

ChatListModel::ChatListModel(ChatManager* manager, QObject* parent)
    : QAbstractListModel(parent), manager_(manager) {

    connect(manager_, &ChatManager::chatAboutToBeCreated,
            this, [this](int row) {
        beginInsertRows(QModelIndex(), row, row);
    });

    connect(manager_, &ChatManager::chatCreated,
            this, [this]() {
        endInsertRows();
    });

    connect(manager_, &ChatManager::chatTitleChanged,
            this, [this](const QString& chatId) {
        for (int row = 0, sz = rowCount(); row < sz; ++row) {
            auto* chat = manager_->chatAt(row);
            if (chat->id == chatId) {
                const QModelIndex idx = index(row, 0);
                emit dataChanged(idx, idx, {Qt::DisplayRole});
                break;
            }
        }
    });
}

int ChatListModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return manager_->chatCount();
}

QVariant ChatListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.column() != 0) {
        return {};
    }

    auto* chat = manager_->chatAt(index.row());

    if (!chat) {
        return {};
    }
    if (role == Qt::DisplayRole) {
        return chat->title;
    } else if (role == Qt::UserRole) {
        return chat->id;
    }
    return {};
}
