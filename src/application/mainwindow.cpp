#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "ui/documents_widget.hpp"
#include "ui/chat_widget.hpp"
#include "ui/sidebar_widget.hpp"
#include "assistant_service.hpp"
#include "application/document_import_queue.hpp"
#include "chat_manager.hpp"
#include "chat_list_model.hpp"
#include "documents/document_list_model.hpp"
#include "server_status_widget.hpp"
#include "ui_style.hpp"

#include <QHBoxLayout>
#include <QStatusBar>
#include <QStackedWidget>
#include <QIcon>
#include <QVBoxLayout>
#include <QCloseEvent>
#include <QTimer>
#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QMap>
#include <memory>

#include <QFileDialog>
#include <QMessageBox>
#include <QDebug>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dwmapi.h>
#endif

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    // Main Window
    ui->setupUi(this);
    setWindowTitle("Local Assistant");
    setWindowIcon(QIcon(":/icons/logo-dark.svg"));

#ifdef Q_OS_WIN
    // Black title.
    constexpr DWORD useImmersiveDarkMode = 20;
    const BOOL darkMode = TRUE;
    const HRESULT titleBarResult = DwmSetWindowAttribute(
        reinterpret_cast<HWND>(winId()), useImmersiveDarkMode,
        &darkMode, sizeof(darkMode));
    if (FAILED(titleBarResult)) {
        qWarning() << "Dark title bar is not supported:" << titleBarResult;
    }
#endif

    resize(1200, 800);
    setMinimumSize(1100, 680);

    ui->menuBar->hide();
    ui->mainToolBar->hide();

    setStyleSheet(UiStyle::applicationStyleSheet());
    statusBar()->hide();

    // Other widgets
    auto* service = new AssistantService(this);
    service_ = service;
    auto* chatManager = new ChatManager(service, this);
    auto* chatListModel = new ChatListModel(chatManager, chatManager);
    auto* documentListModel = new DocumentListModel(
                service->documentManager(),
                service->documentManager());


    centralWidget()->setObjectName("mainSurface");
    auto* rootLayout = new QHBoxLayout(centralWidget());
    rootLayout->setContentsMargins(0, 0, 0, 0); rootLayout->setSpacing(0);

    auto* sidebar = new SidebarWidget(chatListModel);
    auto* serverStatus = new ServerStatusWidget;
    sidebar->setStatusWidget(serverStatus);

    auto* workspace = new QWidget;
    workspace->setObjectName("workspace");
    auto* workspaceLayout = new QVBoxLayout(workspace);
    workspaceLayout->setContentsMargins(0, 0, 0, 0); workspaceLayout->setSpacing(0);
    auto* progressBanner = new QWidget;
    progressBanner->setObjectName("documentProgressBanner");
    progressBanner->setAttribute(Qt::WA_StyledBackground, true);
    progressBanner->setStyleSheet("QWidget#documentProgressBanner { background: #242527; border-bottom: 1px solid #363739; }");
    auto* progressLayout = new QHBoxLayout(progressBanner);
    progressLayout->setContentsMargins(22, 6, 18, 6);
    auto* progressLabel = new QLabel;
    progressLabel->setObjectName("documentProgress"); progressLabel->setTextFormat(Qt::PlainText);
    progressLabel->setWordWrap(true); progressLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    progressLabel->setStyleSheet("color: #A5A6AA; font-size: 12px;");
    auto* cancelDocument = new QPushButton(QStringLiteral("Отмена"));
    cancelDocument->setObjectName("cancelDocument");
    progressLayout->addWidget(progressLabel, 1); progressLayout->addWidget(cancelDocument);
    progressBanner->hide(); workspaceLayout->addWidget(progressBanner);
    auto* progressDismiss = new QTimer(this); progressDismiss->setSingleShot(true);
    connect(progressDismiss, &QTimer::timeout, progressBanner, &QWidget::hide);

    auto* pages = new QStackedWidget;
    auto* chatPage = new ChatWidget;

    pages->addWidget(chatPage);

    auto* documentsPage = new DocumentsWidget(documentListModel);
    pages->addWidget(documentsPage);

    rootLayout->addWidget(sidebar);
    workspaceLayout->addWidget(pages, 1);
    rootLayout->addWidget(workspace, 1);
    auto toggleSidebar = [sidebar]() { sidebar->setVisible(!sidebar->isVisible()); };
    connect(sidebar, &SidebarWidget::collapseRequested, this, toggleSidebar);
    connect(chatPage, &ChatWidget::sidebarToggleRequested, this, toggleSidebar);
    connect(documentsPage, &DocumentsWidget::sidebarToggleRequested, this, toggleSidebar);

    serverStatus->setChatStatus(service->chatServerStatus());
    serverStatus->setEmbeddingStatus(service->embeddingServerStatus());
    connect(service, &AssistantService::serverStatusChanged, serverStatus,
            [serverStatus](const QString& name, const QString& text) {
        if (name == "Qwen") {
            serverStatus->setChatStatus(text);
        } else if (name == "PostgreSQL") {
            serverStatus->setStorageStatus(text);
        } else {
            serverStatus->setEmbeddingStatus(text);
        }
    });
    connect(service, &AssistantService::serverLogMessage,
            serverStatus, &ServerStatusWidget::appendLog);
    connect(service, &AssistantService::documentProgress, this,
            [progressLabel, progressBanner, cancelDocument, progressDismiss](const QString& text, bool canCancel) {
        progressDismiss->stop(); progressLabel->setText(text);
        progressBanner->setVisible(!text.isEmpty()); cancelDocument->setVisible(canCancel);
        if (!canCancel && (text == QStringLiteral("Документ сохранён и готов к поиску") ||
                           text == QStringLiteral("Индексация отменена"))) progressDismiss->start(8000);
    });
    connect(cancelDocument, &QPushButton::clicked, service, &AssistantService::cancelDocument);
    auto updateControls = [service, chatManager, chatPage, documentsPage]() {
        const auto* chat = chatManager->currentChat();
        const bool ready = service->modelsReady() && chat && service->isChatReady(chat->id);
        const bool idle = !service->isBusy() && !chatManager->isBusy();
        chatPage->setSendEnabled(ready && idle);
        chatPage->setUploadEnabled(ready && idle);
        documentsPage->setUploadEnabled(service->storage()->isReady() && chat && service->isChatReady(chat->id));
        chatPage->setWaitingForAnswer(chat && chatManager->isAwaitingAnswer(chat->id));
    };
    connect(chatManager, &ChatManager::operationChanged, this, updateControls);
    connect(chatManager, &ChatManager::currentChatChanged, this, updateControls);
    connect(service, &AssistantService::modelsReadyChanged, this,
            [updateControls](bool) { updateControls(); });
    connect(service, &AssistantService::busyChanged, this,
            [updateControls](bool) { updateControls(); });
    updateControls();
    auto updateImports = [service,chatManager,documentsPage]() {
        const auto* chat = chatManager->currentChat();
        documentsPage->setImportJobs(service->imports()->jobs(), chat ? chat->id : QString());
    };
    connect(service->imports(), &DocumentImportQueue::changed, this, updateImports);
    connect(chatManager, &ChatManager::currentChatChanged, this, updateImports);
    connect(documentsPage, &DocumentsWidget::importActionRequested, service->imports(), &DocumentImportQueue::control);
    connect(service, &AssistantService::serversStopped, this, [this]() {
        shutdownComplete_ = true;
        QTimer::singleShot(0, this, [this]() { close(); });
    });
    connect(qApp, &QCoreApplication::aboutToQuit, service, &AssistantService::stopServers);
    QTimer::singleShot(0, service, &AssistantService::startServers);

    // Navigation
    connect(sidebar, &SidebarWidget::chatPageRequested,
            this, [pages, chatPage]() {
        pages->setCurrentWidget(chatPage);
    });

    connect(sidebar, &SidebarWidget::documentsPageRequested,
            this, [pages, documentsPage]() {
        pages->setCurrentWidget(documentsPage);
    });
    connect(chatPage, &ChatWidget::documentsPageRequested, this, [pages, documentsPage]() {
        pages->setCurrentWidget(documentsPage);
    });
    connect(documentsPage, &DocumentsWidget::chatPageRequested, this, [pages, chatPage]() {
        pages->setCurrentWidget(chatPage);
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
    connect(sidebar, &SidebarWidget::deleteChatRequested, chatManager, &ChatManager::deleteChat);

    connect(chatPage, &ChatWidget::sendRequested,
            this, [chatPage, chatManager](const QString& text) {
        if (chatManager->sendMessage(text, chatPage->attachmentPaths())) {
            chatPage->clearInput();
        }
    });

    // Refresh messages in chat.
    auto refreshMessages = [chatManager, chatPage](bool scrollToBottom) {
        const auto* chat = chatManager->currentChat();
        if (!chat) {
            chatPage->setChatTitle({});
            chatPage->setMessages({});
            chatPage->setContextText({});
            return;
        }
        chatPage->setChatTitle(chat->title);
        chatPage->setMessages(chat->messages, scrollToBottom);
        chatPage->setStreamingAnswer(chatManager->pendingAnswer(chat->id));
        QString sources;
        for (auto it = chat->messages.rbegin(); it != chat->messages.rend(); ++it) {
            if (it->role == MessageRole::User) { sources = it->contextSnapshot; break; }
        }
        chatPage->setContextText(sources);
    };
    connect(chatManager, &ChatManager::chatTitleChanged, this, [chatManager, chatPage](const QString& id) {
        const auto* chat = chatManager->currentChat();
        if (chat && chat->id == id) chatPage->setChatTitle(chat->title);
    });

    auto drafts = std::make_shared<QMap<QString, QString>>();
    auto attachmentDrafts = std::make_shared<QMap<QString, QStringList>>();
    auto displayedChat = std::make_shared<QString>();
    connect(chatManager, &ChatManager::currentChatChanged,
            this, [sidebar, pages, chatPage, refreshMessages, drafts, attachmentDrafts, displayedChat](const QString& chatId) {
        if (chatId != *displayedChat) {
            if (!displayedChat->isEmpty()) {
                (*drafts)[*displayedChat] = chatPage->inputText();
                (*attachmentDrafts)[*displayedChat] = chatPage->attachmentPaths();
            }
            chatPage->setInputText(drafts->value(chatId));
            chatPage->setAttachmentPaths(attachmentDrafts->value(chatId));
            chatPage->showNotification(QString());
            *displayedChat = chatId;
        }
        sidebar->selectChat(chatId);
        pages->setCurrentWidget(chatPage);
        refreshMessages(true);
    });
    connect(chatManager, &ChatManager::chatRemoved, this,
            [drafts, attachmentDrafts, displayedChat, chatPage, chatManager, sidebar](const QString& id) {
        drafts->remove(id);
        attachmentDrafts->remove(id);
        if (*displayedChat == id) { displayedChat->clear(); chatPage->clearInput(); }
        if (const auto* chat = chatManager->currentChat()) sidebar->selectChat(chat->id);
    });
    connect(chatManager, &ChatManager::notification, chatPage, &ChatWidget::showNotification);

    connect(chatManager, &ChatManager::messageAdded,
            this, [chatManager, refreshMessages](const QString& chatId, const QString& messageId) {
        const auto* chat = chatManager->currentChat();
        if (chat && chat->id == chatId) {
            refreshMessages(false);
        }
    });

    connect(chatManager, &ChatManager::answerProgress, this,
            [chatManager, chatPage](const QString& chatId, const QString& text) {
        const auto* chat = chatManager->currentChat();
        if (chat && chat->id == chatId) chatPage->setStreamingAnswer(text);
    });

    // Load document
    auto openDocument = [this, service, chatManager]() {
        const QStringList paths = QFileDialog::getOpenFileNames(
            this,
            tr("Выберите документ или изображение"),
            QString(),
            tr("Документы и изображения (*.txt *.pdf *.docx *.doc *.png *.jpg *.jpeg *.webp *.bmp);;"
               "Документы (*.txt *.pdf *.docx *.doc);;Изображения (*.png *.jpg *.jpeg *.webp *.bmp)"));

        for (const auto& path : paths) service->loadDocument(path);
    };

    connect(chatPage, &ChatWidget::documentUploadRequested, this, [this, chatPage]() {
        const auto selected = QFileDialog::getOpenFileNames(this, QStringLiteral("Прикрепить к сообщению"), {},
            QStringLiteral("Документы и изображения (*.txt *.pdf *.docx *.doc *.png *.jpg *.jpeg *.webp *.bmp)"));
        chatPage->addAttachmentPaths(selected);
    });

    connect(documentsPage, &DocumentsWidget::documentUploadRequested,
            this, openDocument);

    // Notifications
    connect(service, &AssistantService::documentLoaded,
            this, [chatPage, chatManager, service](const QString& fileName, int textLength) {
        const auto* chat = chatManager->currentChat();
        if (!chat || chat->id != service->operationChatId()) return;
        chatPage->showNotification(
            QString("Файл прочитан: %1 · %2 символов")
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
        QMessageBox::warning(this, QStringLiteral("Local Assistant"), message);
    });

    // Sources are refreshed from the selected chat's saved message snapshot.

    // Computer info
    connect(service, &AssistantService::systemStatusReady, serverStatus, &ServerStatusWidget::setSystemUsage);

    // Diagnostic: chat created
    connect(chatManager, &ChatManager::chatCreated,
            this, [this](const QString& chatId, const QString& title){
        qDebug() << "Chat id =" << chatId << ", title =" << title;
    });

}

MainWindow::~MainWindow()
{
    service_->stopServers();
    delete ui;
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (shutdownComplete_) {
        QMainWindow::closeEvent(event);
        return;
    }
    // Keep the event loop alive until both listening processes have exited.
    event->ignore();
    if (!shutdownRequested_) {
        shutdownRequested_ = true;
        service_->stopServers();
    }
}
