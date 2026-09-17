#include "sidebar_widget.hpp"
#include "chat_list_model.hpp"

#include <QVBoxLayout>
#include <QButtonGroup>
#include <QSignalBlocker>
#include <QItemSelectionModel>

SidebarWidget::SidebarWidget(QAbstractItemModel* chatModel, QWidget* parent) : QWidget(parent) {
    this->setObjectName("sidebar");
    this->setStyleSheet(
        "QWidget#sidebar {"
        " background-color: #202126;"
        " border-radius: 16px;"
        "}"
        "QPushButton {"
        " background-color: transparent;"
        " color: #ECEEF2;"
        " border: 1px solid transparent;"
        " border-radius: 8px;"
        " padding: 10px 12px;"
        " text-align: left;"
        " font-size: 15px;"
        "}"
        "QPushButton:hover { background-color: #2C2F36; }"
        "QPushButton:pressed { background-color: #353944; }"
        "QPushButton:checked {"
        " background-color: #293B59;"
        " color: #DCEAFF;"
        "}"
        "QPushButton:checked:hover {"
        " background-color: #32496D;"
        "}"
        "QPushButton:focus { border-color: #3B82F6; }"
    );
    this->setFixedWidth(240);
    auto* sidebarLayout = new QVBoxLayout(this);
    sidebarLayout->setContentsMargins(12, 12, 12, 12);
    sidebarLayout->setSpacing(8);

    QPushButton *btnNewChat = new QPushButton("New chat");
    btnNewChat->setToolTip("Create new chat");
    btnNewChat->setMinimumHeight(44);

    btnShowChat_ = new QPushButton("Chat");
    btnShowChat_->setMinimumHeight(44);
    btnShowDocuments_ = new QPushButton("Documents");
    btnShowDocuments_->setMinimumHeight(44);

    btnShowChat_->setCheckable(true);
    btnShowDocuments_->setCheckable(true);

    auto* navigationGroup = new QButtonGroup(this);
    navigationGroup->setExclusive(true);
    navigationGroup->addButton(btnShowChat_);
    navigationGroup->addButton(btnShowDocuments_);

    btnShowChat_->setChecked(true);

    chatListView_ = new QListView(this);
    chatListView_->setModel(chatModel);
    chatListView_->setStyleSheet(
        "QListView {"
        " background-color: transparent;"
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

    sidebarLayout->addWidget(btnNewChat);
    sidebarLayout->addWidget(btnShowChat_);
    sidebarLayout->addWidget(btnShowDocuments_);
    sidebarLayout->addWidget(chatListView_, 3);

    connect(btnNewChat, &QPushButton::clicked,
           this, &SidebarWidget::newChatRequested);

    connect(btnShowChat_, &QPushButton::clicked,
           this, &SidebarWidget::chatPageRequested);

    connect(btnShowDocuments_, &QPushButton::clicked,
           this, &SidebarWidget::documentsPageRequested);

    connect(chatListView_->selectionModel(),
            &QItemSelectionModel::currentChanged,
            this, [this](const QModelIndex& current,
                         const QModelIndex&) {
        if (!current.isValid()) {
            return;
        }

        const QString id = current.data(Qt::UserRole).toString();
        emit chatSelected(id);
    });

    connect(chatListView_, &QListView::clicked,
            this, &SidebarWidget::chatPageRequested);

}

void SidebarWidget::setChatPageActive(bool active) {
    btnShowChat_->setChecked(active);
    btnShowDocuments_->setChecked(!active);
}

void SidebarWidget::selectChat(const QString& chatId) {
    auto* model = chatListView_->model();

    for (int row = 0; row < model->rowCount(); ++row) {
        const QModelIndex idx = model->index(row, 0);

        if (idx.data(Qt::UserRole).toString() == chatId) {
            const QSignalBlocker blocker(
                chatListView_->selectionModel());

            chatListView_->setCurrentIndex(idx);
            chatListView_->scrollTo(idx);
            return;
        }
    }
}
