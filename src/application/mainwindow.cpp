#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "ui/documents_widget.hpp"
#include "ui/chat_widget.hpp"
#include "ui/sidebar_widget.hpp"
#include "assistant_service.hpp"
#include "chat_manager.hpp"
#include "chat_list_model.hpp"
#include "documents/document_list_model.hpp"

#include <QHBoxLayout>
#include <QStatusBar>
#include <QStackedWidget>

#include <QFileDialog>
#include <QMessageBox>
#include <QDebug>

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    auto* service = new AssistantService(this);
    auto* chatManager = new ChatManager(service, this);
    auto* chatListModel = new ChatListModel(chatManager, chatManager);
    auto* documentListModel = new DocumentListModel(
                service->documentManager(),
                service->documentManager());

    resize(1200, 800);
    setMinimumSize(800, 550);

    centralWidget()->setObjectName("mainSurface");
    centralWidget()->setStyleSheet(
                "QWidget#mainSurface { background-color: #18191C; }"
                );

    auto* rootLayout = new QHBoxLayout(centralWidget());
    rootLayout->setContentsMargins(12, 12, 12, 12);
    rootLayout->setSpacing(12);

    auto* sidebar = new SidebarWidget(chatListModel);

    auto* pages = new QStackedWidget;
    auto* chatPage = new ChatWidget;

    pages->addWidget(chatPage);

    auto* documentsPage = new DocumentsWidget(documentListModel);
    pages->addWidget(documentsPage);

    rootLayout->addWidget(sidebar);
    rootLayout->addWidget(pages, 1);

    // Navigation
    connect(sidebar, &SidebarWidget::chatPageRequested,
            this, [pages, chatPage]() {
        pages->setCurrentWidget(chatPage);
    });

    connect(sidebar, &SidebarWidget::documentsPageRequested,
            this, [pages, documentsPage]() {
        pages->setCurrentWidget(documentsPage);
    });

    connect(pages, &QStackedWidget::currentChanged,
            sidebar, [sidebar, pages, chatPage](int) {
        sidebar->setChatPageActive(
            pages->currentWidget() == chatPage);
    });

    sidebar->setChatPageActive(pages->currentWidget() == chatPage);

    // Chats creation, select and send msg using manager.
    connect(sidebar, &SidebarWidget::newChatRequested,
            chatManager, [chatManager]() {
        chatManager->createChat();
    });

    connect(sidebar, &SidebarWidget::chatSelected,
            chatManager, [chatManager](const QString& id) {
        chatManager->selectChat(id);
    });

    connect(chatPage, &ChatWidget::sendRequested,
            this, [chatPage, chatManager](const QString& text) {
        if (chatManager->sendMessage(text)) {
            chatPage->clearInput();
        }
    });

    // Refresh messages in chat.
    auto refreshMessages = [chatManager, chatPage]() {
        const auto* chat = chatManager->currentChat();
        if (!chat) {
            chatPage->setMessages({});
            return;
        }
        chatPage->setMessages(chat->messages);
    };

    connect(chatManager, &ChatManager::currentChatChanged,
            this, [sidebar, pages, chatPage, refreshMessages]
            (const QString& chatId) {
        sidebar->selectChat(chatId);
        pages->setCurrentWidget(chatPage);
        refreshMessages();
    });

    connect(chatManager, &ChatManager::messageAdded,
            this, [chatManager, refreshMessages](const QString& chatId, const QString& messageId) {
        const auto* chat = chatManager->currentChat();
        if (chat && chat->id == chatId) {
            refreshMessages();
        }
    });

    // Load document
    auto openDocument = [this, service]() {
        const QString path = QFileDialog::getOpenFileName(
            this,
            tr("Select document"),
            QString(),
            tr("Text documents (*.txt)"));

        if (path.isEmpty()) {
            return;
        }

        service->loadDocument(path);
    };

    connect(chatPage, &ChatWidget::documentUploadRequested,
            this, openDocument);

    connect(documentsPage, &DocumentsWidget::documentUploadRequested,
            this, openDocument);

    // Notifications
    connect(service, &AssistantService::documentLoaded,
            this, [chatPage](const QString& fileName, int textLength) {
        chatPage->showNotification(
            QString("Документ прочитан: %1 · %2 символов")
                .arg(fileName)
                .arg(textLength)
        );
    });

    connect(service, &AssistantService::errorOccurred,
            this, [chatPage](const QString& message) {
        chatPage->showNotification(
            QString("Ошибка: %1").arg(message)
        );
    });

    connect(chatManager, &ChatManager::errorOccurred,
            this, [this](const QString& message) {
        QMessageBox::warning(this, "Couldn't send", message);
    });

    // Sources
    connect(service, &AssistantService::retrievalReady,
            this, [chatPage](const std::vector<VectorStore::SearchResult> &results) {
        if (results.empty()) {
            chatPage->setContextText("No fragments");
            return;
        }
        QString ans;
        for (const auto& r : results) {
            ans += "Источник: " + r.chunk.documentId + "\nПозиция: " + QString::number(r.chunk.startOffset) +
                    "\nSimilarity: " + QString::number(r.score) + "\n\n" + r.chunk.text + "\n\n";

        }
        chatPage->setContextText(ans);
    });

    // Computer info
    connect(service, &AssistantService::systemStatusReady,
            this, [this](double cpuPercent, double memoryPercent) {
       statusBar()->showMessage(QString("Usage: CPU = %1% | RAM = %2%")
                                .arg(cpuPercent).arg(memoryPercent));
    });

    // Diagnostic: chat created
    connect(chatManager, &ChatManager::chatCreated,
            this, [this](const QString& chatId, const QString& title){
        qDebug() << "Chat id =" << chatId << ", title =" << title;
    });

}

MainWindow::~MainWindow()
{
    delete ui;
}
