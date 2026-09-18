#pragma once

#include "chat_manager.hpp"

#include <QAbstractListModel>
#include <QVariant>


class ChatListModel : public QAbstractListModel {
    Q_OBJECT
    ChatManager* manager_;
public:
    explicit ChatListModel(ChatManager* manager, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;

    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
};

