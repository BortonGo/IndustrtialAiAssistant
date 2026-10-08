#include "sidebar_widget.hpp"
#include "chat_list_model.hpp"
#include "ui_style.hpp"

#include <QVBoxLayout>
#include <QSignalBlocker>
#include <QItemSelectionModel>
#include <QMenu>
#include <QPainter>
#include <QPersistentModelIndex>
#include <QLabel>
#include <QToolButton>

SidebarWidget::SidebarWidget(QAbstractItemModel* chatModel, QWidget* parent) : QWidget(parent) {
    this->setObjectName("sidebar");
    setAttribute(Qt::WA_StyledBackground, true);
    this->setStyleSheet(
        "QWidget#sidebar {"
        " background-color: #202122;"
        " border-right: 1px solid #363739;"
        "}"
        "QPushButton {"
        " background-color: transparent;"
        " color: #ECECEE;"
        " border: 1px solid transparent;"
        " border-radius: 8px;"
        " padding: 8px 10px;"
        " text-align: left;"
        " font-size: 14px;"
        "}"
        "QPushButton:hover { background-color: #2D2E30; }"
        "QPushButton:pressed { background-color: #323437; }"
        "QPushButton:checked {"
        " background-color: #323437;"
        " color: #ECECEE;"
        "}"
        "QPushButton:checked:hover {"
        " background-color: #323437;"
        "}"
        "QPushButton:focus { border-color: #CCD7E8; }"
    );
    this->setFixedWidth(226);
    auto* sidebarLayout = new QVBoxLayout(this);
    sidebarLayout_ = sidebarLayout;
    sidebarLayout->setContentsMargins(12, 14, 12, 10);
    sidebarLayout->setSpacing(4);

    auto* brand = new QHBoxLayout;
    brand->setContentsMargins(5, 0, 0, 0); brand->setSpacing(8);
    auto* emblem = new QLabel;
    emblem->setObjectName("sidebarEmblem");
    emblem->setPixmap(QIcon(":/icons/logo-dark.svg").pixmap(22, 22));
    emblem->setFixedSize(22, 22);
    auto* name = new QLabel("Local Assistant");
    name->setStyleSheet("font-size: 14px; font-weight: 600; color: #ECECEE;");
    auto* collapse = new QToolButton;
    collapse->setObjectName("collapseSidebar");
    collapse->setIcon(UiStyle::icon(UiStyle::Icon::Sidebar));
    collapse->setToolTip(QStringLiteral("Свернуть панель чатов"));
    collapse->setAccessibleName(collapse->toolTip()); collapse->setFixedSize(28, 32);
    brand->addWidget(emblem); brand->addWidget(name, 1); brand->addWidget(collapse);
    sidebarLayout->addLayout(brand); sidebarLayout->addSpacing(18);
    connect(collapse, &QToolButton::clicked, this, &SidebarWidget::collapseRequested);

    QPushButton *btnNewChat = new QPushButton(QStringLiteral("Новый чат"));
    btnNewChat->setObjectName("newChatButton");
    btnNewChat->setIcon(UiStyle::icon(UiStyle::Icon::NewChat));
    btnNewChat->setMinimumHeight(38);

    btnShowDocuments_ = new QPushButton(QStringLiteral("Документы чата"));
    btnShowDocuments_->setIcon(UiStyle::icon(UiStyle::Icon::Documents));
    btnShowDocuments_->setMinimumHeight(38);
    btnShowDocuments_->setCheckable(true);

    chatListView_ = new QListView(this);
    chatListView_->setObjectName("chatList");
    chatListView_->setTextElideMode(Qt::ElideRight);
    chatListView_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    chatListView_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(chatModel, &QAbstractItemModel::rowsAboutToBeRemoved, this, [this]() { changingRows_ = true; });
    chatListView_->setModel(chatModel);
    connect(chatModel, &QAbstractItemModel::rowsRemoved, this, [this]() { changingRows_ = false; });
    connect(chatListView_, &QWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        const QPersistentModelIndex index(chatListView_->indexAt(position));
        if (!index.isValid()) return;
        const auto id = index.data(ChatListModel::ChatIdRole).toString();
        QMenu menu(this);
        menu.setObjectName("chatContextMenu");
        menu.setStyleSheet(
            "QMenu { background: #242527; border: 1px solid #363739; border-radius: 10px; padding: 6px; }"
            "QMenu::item { color: #F49C9C; padding: 10px 18px 10px 32px; border-radius: 6px; }"
            "QMenu::item:selected { background: #443036; color: #FFD0D0; }"
            "QMenu::item:disabled { color: #9198A5; }"
            "QMenu::icon { left: 10px; }");
        QPixmap icon(20, 20); icon.fill(Qt::transparent);
        {
            QPainter painter(&icon); painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(QPen(QColor("#F49C9C"), 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.drawLine(4, 6, 16, 6); painter.drawLine(8, 3, 12, 3);
            painter.drawLine(8, 3, 7, 6); painter.drawLine(12, 3, 13, 6);
            QPolygonF outline; outline << QPointF(5, 8) << QPointF(6, 17) << QPointF(14, 17) << QPointF(15, 8);
            painter.drawPolyline(outline);
            painter.drawLine(8, 9, 8, 14); painter.drawLine(12, 9, 12, 14);
        }
        auto* remove = menu.addAction(QIcon(icon), QStringLiteral("Удалить чат"));
        remove->setEnabled(index.data(ChatListModel::CanDeleteRole).toBool());
        menu.addAction(remove->isEnabled() ? QStringLiteral("Вместе с документами чата") :
                       QStringLiteral("Дождитесь завершения операции"))->setEnabled(false);
        if (menu.exec(chatListView_->viewport()->mapToGlobal(position)) == remove &&
            index.isValid() && index.data(ChatListModel::CanDeleteRole).toBool())
            emit deleteChatRequested(id);
    });
    chatListView_->setStyleSheet(
        "QListView {"
        " background-color: transparent;"
        " color: #ECECEE;"
        " border: none;"
        " font-size: 14px;"
        " outline: none;"
        "}"
        "QListView::item {"
        " padding: 8px 10px;"
        " margin-bottom: 2px;"
        " border-radius: 7px;"
        "}"
        "QListView::item:hover {"
        " background-color: #2D2E30;"
        "}"
        "QListView::item:selected {"
        " background-color: #323437;"
        " color: #ECECEE;"
        "}"
    );

    sidebarLayout->addWidget(btnNewChat);
    sidebarLayout->addWidget(btnShowDocuments_);
    sidebarLayout->addSpacing(18);
    auto* chatsLabel = new QLabel(QStringLiteral("Чаты"));
    chatsLabel->setStyleSheet("color: #84858A; font-size: 12px; padding: 4px 10px;");
    sidebarLayout->addWidget(chatsLabel);
    sidebarLayout->addWidget(chatListView_, 3);

    connect(btnNewChat, &QPushButton::clicked,
           this, &SidebarWidget::newChatRequested);

    connect(btnShowDocuments_, &QPushButton::clicked,
           this, [this]() {
        btnShowDocuments_->setChecked(true);
        emit documentsPageRequested();
    });

    connect(chatListView_->selectionModel(),
            &QItemSelectionModel::currentChanged,
            this, [this](const QModelIndex& current,
                         const QModelIndex&) {
        if (!current.isValid() || changingRows_) {
            return;
        }

        const QString id = current.data(Qt::UserRole).toString();
        emit chatSelected(id);
    });

    connect(chatListView_, &QListView::clicked,
            this, &SidebarWidget::chatPageRequested);

}

void SidebarWidget::setChatPageActive(bool active) {
    btnShowDocuments_->setChecked(!active);
}

void SidebarWidget::setStatusWidget(QWidget* widget) {
    sidebarLayout_->addWidget(widget);
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
