#pragma once

#include <QObject>
#include <QAbstractItemModel>
#include <QWidget>
#include <QTextEdit>
#include <QListView>
#include <QPushButton>
#include <QString>

class SidebarWidget : public QWidget {
    Q_OBJECT

    QListView* chatListView_;
    QPushButton* btnShowChat_;
    QPushButton* btnShowDocuments_;
    bool changingRows_ = false;
public:
    explicit SidebarWidget(QAbstractItemModel* chatModel,
                           QWidget* parent = nullptr);

    void setChatPageActive(bool active);
    void selectChat(const QString& chatId);

signals:
    void newChatRequested();
    void chatPageRequested();
    void documentsPageRequested();
    void chatSelected(const QString& chatId);
    void deleteChatRequested(const QString& chatId);
};
