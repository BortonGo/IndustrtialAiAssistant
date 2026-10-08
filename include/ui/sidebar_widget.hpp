#pragma once

#include <QObject>
#include <QAbstractItemModel>
#include <QWidget>
#include <QTextEdit>
#include <QListView>
#include <QPushButton>
#include <QString>

class QVBoxLayout;

class SidebarWidget : public QWidget {
    Q_OBJECT

    QListView* chatListView_;
    QPushButton* btnShowDocuments_;
    QVBoxLayout* sidebarLayout_;
    bool changingRows_ = false;
public:
    explicit SidebarWidget(QAbstractItemModel* chatModel,
                           QWidget* parent = nullptr);

    void setChatPageActive(bool active);
    void selectChat(const QString& chatId);
    void setStatusWidget(QWidget* widget);

signals:
    void newChatRequested();
    void collapseRequested();
    void chatPageRequested();
    void documentsPageRequested();
    void chatSelected(const QString& chatId);
    void deleteChatRequested(const QString& chatId);
};
