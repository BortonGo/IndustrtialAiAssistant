#include "chat_list_model.hpp"

ChatListModel::ChatListModel(ChatManager* manager, QObject* parent)
    : QAbstractListModel(parent), manager_(manager) {
    connect(manager_, &ChatManager::chatsAboutToReset, this, [this]() { beginResetModel(); });
    connect(manager_, &ChatManager::chatsReset, this, [this]() { endResetModel(); });
    connect(manager_, &ChatManager::chatActivityChanged, this, [this]() {
        if (rowCount() > 0) emit dataChanged(index(0, 0), index(rowCount() - 1, 0), {Qt::DisplayRole, Qt::ToolTipRole, CanDeleteRole});
    });
    connect(manager_, &ChatManager::chatAboutToBeRemoved, this, [this](int row) { beginRemoveRows({}, row, row); });
    connect(manager_, &ChatManager::chatRemoved, this, [this]() { endRemoveRows(); });

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
        const auto activity = manager_->chatActivity(chat->id);
        return chat->title + (activity.isEmpty() ? QString() : "\n" + activity);
    } else if (role == Qt::ToolTipRole) {
        return chat->title + "\n" + manager_->chatActivity(chat->id);
    } else if (role == Qt::UserRole) {
        return chat->id;
    } else if (role == CanDeleteRole) {
        return manager_->canDeleteChat(chat->id);
    }
    return {};
}
