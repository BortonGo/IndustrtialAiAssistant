#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "assistant_service.hpp"
#include "documents/document.hpp"
#include "chat_manager.hpp"
#include "chat_list_model.hpp"
#include "documents/document_list_model.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTextEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QListView>
#include <QLabel>
#include <QItemSelectionModel>

#include <QFileDialog>
#include <QMessageBox>
#include <QDebug>

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    resize(1200, 800);
    setMinimumSize(800, 550);

    centralWidget()->setObjectName("mainSurface");
    centralWidget()->setStyleSheet(
                "QWidget#mainSurface { background-color: #18191C; }"
                );

    auto* rootLayout = new QHBoxLayout(centralWidget());

    auto* sidebar = new QWidget;
    sidebar->setFixedWidth(220);
    auto* sidebarLayout = new QVBoxLayout(sidebar);

    auto* vl = new QVBoxLayout;

    rootLayout->addWidget(sidebar);
    rootLayout->addLayout(vl, 1);

    auto *hl = new QHBoxLayout;
    hl->setContentsMargins(20,20,20,20);
    hl->setSpacing(16);

    auto *hlAnswers = new QHBoxLayout;
    hlAnswers->setContentsMargins(20,20,20,20);
    hlAnswers->setSpacing(16);

    QTextEdit *textQuestion = new QTextEdit;

    textQuestion->setPlaceholderText("Ask your question");
    textQuestion->setStyleSheet(
                "QTextEdit {"
                " background-color: #24262B;"
                " color: #E8E8ED;"
                " border: 1px solid #393C44;"
                " border-radius: 14px;"
                " padding: 12px;"
                " font-size: 18px;"
                "}"
                );
    textQuestion->setMinimumHeight(80);
    textQuestion->setMaximumHeight(110);

    QTextEdit *textAnswer = new QTextEdit;
    textAnswer->setStyleSheet(
                "QTextEdit {"
                " background-color: #24262B;"
                " color: #E8E8ED;"
                " border: 1px solid #393C44;"
                " border-radius: 14px;"
                " padding: 12px;"
                " font-size: 18px;"
                "}"
                );

    QTextEdit *textContext = new QTextEdit;
    textContext->setStyleSheet(
                "QTextEdit {"
                " background-color: #24262B;"
                " color: #E8E8ED;"
                " border: 1px solid #393C44;"
                " border-radius: 14px;"
                " padding: 12px;"
                " font-size: 18px;"
                "}"
                );

    QPushButton *btnAsk = new QPushButton("↑");
    btnAsk->setFixedSize(44,44);
    btnAsk->setToolTip("Send question");
    btnAsk->setStyleSheet(
                "QPushButton {"
                " background-color: #1E90FF;"
                " color: #E8E8ED;"
                " border: none;"
                " border-radius: 22px;"
                " padding: 0px;"
                " padding-bottom: 4px;"
                " font-size: 26px;"
                "}"
                "QPushButton:hover { background-color: #46A3FF; }"
                "QPushButton:disabled {"
                " background-color: #393C44;"
                " color: #777B85;"
                "}"
                );
    QPushButton *btnLoadDoc = new QPushButton("+");
    btnLoadDoc->setFixedSize(44,44);
    btnLoadDoc->setToolTip("Load document");
    btnLoadDoc->setStyleSheet(
                "QPushButton {"
                " background-color: #393C44;"
                " color: #E8E8ED;"
                " border: none;"
                " border-radius: 22px;"
                " padding: 0px;"
                " padding-bottom: 4px;"
                " font-size: 26px;"
                "}"
                "QPushButton:hover { background-color: #494D57; }"
                );

    QPushButton *btnNewChat = new QPushButton("New chat");
    btnNewChat->setFixedSize(150,44);
    btnNewChat->setToolTip("Create new chat");
    btnNewChat->setStyleSheet(
                "QPushButton {"
                " background-color: #1E90FF;"
                " color: #E8E8ED;"
                " border: none;"
                " border-radius: 15px;"
                " padding: 0px;"
                " padding-bottom: 4px;"
                " font-size: 20px;"
                "}"
                "QPushButton:hover { background-color: #46A3FF; }"
                );

    auto* notificationLabel = new QLabel;
    notificationLabel->setTextFormat(Qt::PlainText);
    notificationLabel->setWordWrap(true);
    notificationLabel->setStyleSheet(
        "QLabel {"
        " color: #B8BCC6;"
        " font-size: 14px;"
        " padding: 6px;"
        "}"
    );
    notificationLabel->hide();

    vl->insertWidget(0, notificationLabel);

    textAnswer->setReadOnly(true);
    textContext->setReadOnly(true);
    hlAnswers->addWidget(textAnswer, 7);
    hlAnswers->addWidget(textContext, 3);
    sidebarLayout->addWidget(btnNewChat);
    vl->addLayout(hlAnswers);
    hl->addWidget(btnLoadDoc);
    hl->addWidget(textQuestion);
    hl->addWidget(btnAsk);
    vl->addLayout(hl);

    auto* service = new AssistantService(this);

    connect(service, &AssistantService::documentLoaded,
            this, [notificationLabel](const QString& fileName, int textLength) {
        notificationLabel->setText(
            QString("Документ прочитан: %1 · %2 символов")
                .arg(fileName)
                .arg(textLength)
        );
        notificationLabel->show();
    });

    connect(service, &AssistantService::errorOccurred,
            this, [notificationLabel](const QString& message) {
        notificationLabel->setText(
            QString("Ошибка: %1").arg(message)
        );
        notificationLabel->show();
    });

    connect(service, &AssistantService::retrievalReady,
            this, [this, textContext](const std::vector<VectorStore::SearchResult> &results) {
        if (results.empty()) {
            textContext->setPlainText("No fragments");
            return;
        }
        QString ans;
        for (const auto& r : results) {
            ans += "Источник: " + r.chunk.documentId + "\nПозиция: " + QString::number(r.chunk.startOffset) +
                    "\nSimilarity: " + QString::number(r.score) + "\n\n" + r.chunk.text + "\n\n";

        }
        textContext->setPlainText(ans);
    });

    connect(service, &AssistantService::systemStatusReady,
            this, [this](double cpuPercent, double memoryPercent) {
       statusBar()->showMessage(QString("Usage: CPU = %1% | RAM = %2%")
                                .arg(cpuPercent).arg(memoryPercent));
    });


    auto* chatManager = new ChatManager(service, this);

    auto refreshMessages = [chatManager, textAnswer]() {
        const auto* chat = chatManager->currentChat();
        if (!chat) {
            textAnswer->clear();
            return;
        }
        QString ans;
        for (const auto& m : chat->messages) {
            if (m.role == MessageRole::User) {
                ans += "You: \n\n" + m.text + "\n\n";
            } else {
                ans += "Assistant: \n\n" + m.text + "\n\n";
            }
        }
        textAnswer->setPlainText(ans);
    };

    connect(btnNewChat, &QPushButton::clicked,
            chatManager, [chatManager]() {
        chatManager->createChat();
    });

    connect(chatManager, &ChatManager::chatCreated,
            this, [this](const QString& chatId, const QString& title){
        qDebug() << "Chat id =" << chatId << ", title =" << title;
    });

    connect(chatManager, &ChatManager::errorOccurred,
            this, [this](const QString& message) {
        QMessageBox::warning(this, "Couldn't send", message);
    });

    auto* chatListModel = new ChatListModel(chatManager, chatManager);
    QListView* chatListView = new QListView(centralWidget());
    chatListView->setModel(chatListModel);
    chatListView->setStyleSheet(
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
    sidebarLayout->addWidget(chatListView, 3);

    auto* documentsLabel = new QLabel("Документы · общие");
    documentsLabel->setStyleSheet(
        "color: #B8BCC6;"
        " font-size: 14px;"
        " padding: 6px;"
    );
    sidebarLayout->addWidget(documentsLabel);

    auto* documentListModel = new DocumentListModel(service->documentManager(), service->documentManager());
    QListView* documentListView = new QListView(centralWidget());
    documentListView->setModel(documentListModel);
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
    sidebarLayout->addWidget(documentListView, 2);

    connect(chatListView->selectionModel(), &QItemSelectionModel::currentChanged,
            chatManager, [chatManager](const QModelIndex& current, const QModelIndex& previous) {
        if (!current.isValid()) {
            return;
        }
        QString id = current.data(Qt::UserRole).toString();
        chatManager->selectChat(id);
    });

    connect(chatManager, &ChatManager::currentChatChanged,
            this, [chatListModel, chatListView, refreshMessages](const QString& chatId){
        for (int i = 0, sz = chatListModel->rowCount(); i < sz; ++i) {
            const QModelIndex idx = chatListModel->index(i, 0);
            QString id = idx.data(Qt::UserRole).toString();
            if (id == chatId) {
                chatListView->setCurrentIndex(idx);
                chatListView->scrollTo(idx);
                refreshMessages();
                break;
            }
        }
    });

    connect(chatManager, &ChatManager::messageAdded,
            this, [chatManager, refreshMessages](const QString& chatId, const QString& messageId) {
        const auto* chat = chatManager->currentChat();
        if (chat && chat->id == chatId) {
            refreshMessages();
        }
    });

    chatManager->createChat();



    connect(btnAsk, &QPushButton::clicked,
        this, [textQuestion, chatManager]() {
        if (chatManager->sendMessage(textQuestion->toPlainText())) {
            textQuestion->clear();
        }
    });

    connect(btnLoadDoc, &QPushButton::clicked,
            this, [this, service]() {
        const QString path = QFileDialog::getOpenFileName(
                    this,
                    tr("Select document"),
                    QString(),
                    tr("Text documents (*.txt)"));
        if (path.isEmpty()) {
            return;
        }
        service->loadDocument(path);
    });

}

MainWindow::~MainWindow()
{
    delete ui;
}

