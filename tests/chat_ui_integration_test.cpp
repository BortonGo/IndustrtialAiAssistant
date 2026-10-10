// Manual Qt/PostgreSQL integration check. Creates its own cluster; no model servers.
#include "application/assistant_service.hpp"
#include "chat/chat_manager.hpp"
#include "chat/chat_list_model.hpp"
#include "ui/chat_widget.hpp"
#include "ui/sidebar_widget.hpp"
#include "ui/thinking_indicator.hpp"
#include "ui/server_status_widget.hpp"
#include "ui/ui_style.hpp"
#include "ui/documents_widget.hpp"
#include "ui/document_comparison_dialog.hpp"
#include "ui/document_tables_dialog.hpp"
#include "ui/document_library_dialog.hpp"
#include "documents/document_list_model.hpp"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QTemporaryDir>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QTimer>
#include <QMenu>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPainter>
#include <QDebug>
#include <QUuid>
#include <QJsonArray>
#include <QScrollBar>
#include <QToolButton>
#include <QTextBrowser>
#include <QPushButton>
#include <QLineEdit>
#include <QClipboard>
#include <QKeyEvent>
#include <QMimeData>
#include <QListWidget>
#include <QStandardPaths>
#include <QDialog>
#include <QPointer>
#include <QTcpServer>
#include <QTextTable>
#include <QTableWidget>
#include <QComboBox>
#include <QFileDialog>
#include <stdexcept>
#include <cstdio>

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void waitFor(const std::function<bool()>& condition) {
    QElapsedTimer deadline; deadline.start();
    while (!condition() && deadline.elapsed() < 30000) {
        QEventLoop loop; QTimer::singleShot(10, &loop, &QEventLoop::quit); loop.exec();
    }
    require(condition(), "Timed out waiting for UI/storage");
}
QJsonObject request(StorageClient* storage, const QString& method, const QJsonObject& params) {
    bool done = false; QString failure; QJsonObject result;
    storage->request(method, params, [&](const QJsonObject& data, const QString& error) {
        result = data; failure = error; done = true;
    });
    waitFor([&]() { return done; });
    require(failure.isEmpty(), qPrintable(failure)); return result;
}
}

int main(int argc, char** argv) {
    std::setvbuf(stderr,nullptr,_IONBF,0);
    QApplication app(argc, argv);
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext&, const QString& message) {
        if (type != QtDebugMsg) std::fprintf(stderr, "%s\n", message.toUtf8().constData());
    });
    if (QApplication::platformName() == "offscreen") {
        QFontDatabase::addApplicationFont("C:/Windows/Fonts/segoeui.ttf");
        QFontDatabase::addApplicationFont("C:/Windows/Fonts/segoeuib.ttf");
    }
    app.setFont(QFont("Segoe UI", 10));
    QDir().mkpath("build-qtcreator/test-artifacts/storage");
    QTemporaryDir data(QDir::current().filePath("build-qtcreator/test-artifacts/storage/ui-XXXXXX"));
    require(data.isValid(), "No isolated directory");
    qputenv("LOCAL_ASSISTANT_STORAGE_DIR", data.path().toLocal8Bit());
    QTcpServer freePort;
    require(freePort.listen(QHostAddress::LocalHost, 0), "No free port for isolated PostgreSQL");
    qputenv("LOCAL_ASSISTANT_STORAGE_PORT", QByteArray::number(freePort.serverPort())); freePort.close();
    AssistantService service;
    ChatManager chats(&service);
    ChatListModel model(&chats);
    QString failure;
    QObject::connect(&chats, &ChatManager::errorOccurred, [&](const QString& error) { failure = error; });
    QObject::connect(service.storage(), &StorageClient::unavailable, [&](const QString& error) {
        std::fprintf(stderr, "STORAGE: %s\n", error.toUtf8().constData()); failure = error;
    });
    try {
        service.storage()->start();
        waitFor([&]() { return chats.currentChat() && service.isChatReady(chats.currentChat()->id); });
        QWidget window;
        window.setObjectName("preview");
        window.setStyleSheet(UiStyle::applicationStyleSheet() + "QWidget#preview { background: #191A1B; }");
        auto* layout = new QHBoxLayout(&window);
        layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
        auto* sidebar = new SidebarWidget(&model);
        auto* page = new ChatWidget;
        auto* services = new ServerStatusWidget;
        sidebar->setStatusWidget(services);
        services->setChatStatus(QStringLiteral("Готов · CUDA"));
        services->setEmbeddingStatus(QStringLiteral("Готов · CUDA"));
        DocumentManager previewManager(DocumentExtractionConfig{});
        Document previewDocument;
        previewDocument.id = "preview"; previewDocument.sourcePath = "C:/docs/Protocol.pdf";
        previewDocument.status = DocumentStatus::TextReady; previewDocument.pendingImages = 38;
        previewDocument.text = "Native text";
        previewManager.restore({previewDocument});
        DocumentListModel previewModel(&previewManager);
        require(previewModel.data(previewModel.index(0, 0), Qt::DisplayRole).toString().contains(QStringLiteral("Текст готов")),
                "Preview readiness is invisible in the document list");
        require(previewModel.data(previewModel.index(0, 0), Qt::ToolTipRole).toString().contains("38"),
                "Pending image count missing from document tooltip");
        DocumentsWidget documentsPage(&previewModel);
        documentsPage.setComparisonEnabled(true);
        auto* compareButton = documentsPage.findChild<QPushButton*>("compareDocuments");
        auto* tablesButton = documentsPage.findChild<QPushButton*>("documentTablesButton");
        auto* publishButton = documentsPage.findChild<QPushButton*>("publishLibraryDocument");
        QString tablesDocumentId;
        QObject::connect(&documentsPage, &DocumentsWidget::tablesRequested, [&](const QString& id) { tablesDocumentId = id; });
        auto* documentList = documentsPage.findChild<QListView*>("chatDocuments");
        auto selectDocument = [&](int row) {
            documentList->selectionModel()->select(previewModel.index(row, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        };
        require(!compareButton->isEnabled(), "Comparison allowed with no selection");
        auto secondDocument = previewDocument; secondDocument.id = "second"; secondDocument.status = DocumentStatus::Ready; secondDocument.pendingImages=0;
        auto thirdDocument = secondDocument; thirdDocument.id = "third";
        previewManager.restore({previewDocument, secondDocument, thirdDocument});
        selectDocument(1); require(!compareButton->isEnabled(), "Comparison allowed with one document");
        require(tablesButton->isEnabled(), "Tables unavailable for a selected ready document");
        require(publishButton->isEnabled(), "Ready document cannot be published to the library");
        tablesButton->click(); require(tablesDocumentId == "second", "Table viewer has wrong document ID");
        selectDocument(0); require(compareButton->isEnabled(), "Partial text and ready document cannot be compared");
        require(!tablesButton->isEnabled(), "Tables enabled with ambiguous document selection");
        QStringList comparisonIds;
        QObject::connect(&documentsPage, &DocumentsWidget::compareRequested, [&](const QStringList& ids) { comparisonIds = ids; });
        compareButton->click();
        require(comparisonIds == QStringList({"preview", "second"}), "Document comparison order is unstable");
        documentsPage.setComparisonEnabled(false); require(!compareButton->isEnabled(), "Comparison enabled without storage");
        documentsPage.setComparisonEnabled(true);
        previewManager.setDocumentStatus("second", DocumentStatus::Indexing);
        require(!compareButton->isEnabled(), "Unpublished indexing document can be compared");
        previewManager.setDocumentStatus("second", DocumentStatus::Ready);
        require(compareButton->isEnabled(), "Readiness change did not enable comparison");
        selectDocument(2); require(!compareButton->isEnabled(), "Comparison allowed with three documents");
        previewManager.restore({previewDocument});
        selectDocument(0); require(!publishButton->isEnabled(), "Partial OCR result can be published to the library");
        documentList->clearSelection();
        require(!compareButton->isEnabled() && documentList->selectionModel()->selectedRows().isEmpty(), "Chat reload retained comparison selection");
        require(!tablesButton->isEnabled(), "Chat reload retained table action");
        QJsonArray importJobs{
            QJsonObject{{"id","import-a"},{"chatId","chat-a"},{"sourcePath","C:/docs/Protocol.pdf"},{"status","running"},{"progress",QStringLiteral("Изображения: 12/50")}},
            QJsonObject{{"id","import-b"},{"chatId","chat-b"},{"sourcePath","C:/docs/Other.docx"},{"status","paused"}}
        };
        QString importAction, importId;
        QObject::connect(&documentsPage, &DocumentsWidget::importActionRequested, [&](const QString& id, const QString& action) { importId = id; importAction = action; });
        documentsPage.setImportJobs(importJobs, "chat-a");
        auto* importList = documentsPage.findChild<QListWidget*>("documentImportQueue");
        require(importList && importList->count() == 1 && importList->item(0)->text().contains("Protocol.pdf"), "Queue leaked another chat's jobs");
        documentsPage.findChild<QPushButton*>("pauseImport")->click();
        require(importId == "import-a" && importAction == "paused", "Pause button routes to wrong job");
        require(!documentsPage.findChild<QPushButton*>("resumeImport")->isEnabled(), "Running job should not offer resume");
        documentsPage.setObjectName("workspace"); documentsPage.setAttribute(Qt::WA_StyledBackground, true);
        documentsPage.setStyleSheet(UiStyle::applicationStyleSheet()); documentsPage.resize(850, 600);
        documentsPage.show(); app.processEvents();
        documentsPage.grab().save("build-qtcreator/import-queue-preview.png"); documentsPage.hide();
        documentsPage.setImportJobs(importJobs, "chat-b");
        documentsPage.findChild<QPushButton*>("resumeImport")->click();
        require(importId == "import-b" && importAction == "queued", "Resume button routes to wrong job");
        documentsPage.findChild<QPushButton*>("cancelImport")->click();
        require(importAction == "cancelled", "Queue cancel action is missing");
        documentsPage.setImportJobs({}, "chat-a");
        require(importList->isHidden(), "Empty queue still occupies the documents page");
        bool backToChat = false, requestedDocuments = false;
        QObject::connect(&documentsPage, &DocumentsWidget::chatPageRequested, [&]() { backToChat = true; });
        QObject::connect(page, &ChatWidget::documentsPageRequested, [&]() { requestedDocuments = true; });
        for (auto* button : documentsPage.findChildren<QPushButton*>())
            if (button->text() == QStringLiteral("К переписке")) button->click();
        require(backToChat, "Documents page has no return action");
        page->findChild<QPushButton*>("toggleSources")->click();
        for (auto* button : page->findChildren<QPushButton*>())
            if (button->text() == QStringLiteral("Документы чата")) button->click();
        require(requestedDocuments, "Sources panel cannot open documents");
        page->findChild<QPushButton*>("toggleSources")->click();
        services->setStorageStatus(QStringLiteral("Готова · PostgreSQL"));
        services->appendLog("Qwen", QStringLiteral("Модель загружена"));
        layout->addWidget(sidebar); layout->addWidget(page, 1);
        const auto toggleSidebar = [sidebar]() { sidebar->setVisible(!sidebar->isVisible()); };
        QObject::connect(sidebar, &SidebarWidget::collapseRequested, toggleSidebar);
        QObject::connect(page, &ChatWidget::sidebarToggleRequested, toggleSidebar);
        QObject::connect(sidebar, &SidebarWidget::chatSelected, &chats, &ChatManager::selectChat);
        QObject::connect(sidebar, &SidebarWidget::deleteChatRequested, &chats, &ChatManager::deleteChat);
        QObject::connect(&chats, &ChatManager::currentChatChanged, [&](const QString& id) {
            sidebar->selectChat(id); page->setMessages(chats.currentChat()->messages);
            page->setWaitingForAnswer(chats.isAwaitingAnswer(id));
        });
        QObject::connect(&chats, &ChatManager::chatRemoved, [&]() {
            if (chats.currentChat()) sidebar->selectChat(chats.currentChat()->id);
        });
        window.resize(1100, 720); window.show();
        const auto* emblem = sidebar->findChild<QLabel*>("sidebarEmblem");
        require(emblem && emblem->pixmap() && !emblem->pixmap()->isNull(), "Original emblem missing from sidebar");
        auto* summary = services->findChild<QPushButton*>("serverSummary");
        auto* details = services->findChild<QWidget*>("serverDetails");
        require(summary && details && !details->isVisible(), "Service details should start collapsed");
        require(summary->text().contains(QStringLiteral("Модели готовы")), "Ready summary incorrect");
        summary->click(); require(details->isVisible(), "Service details inaccessible");
        services->findChild<QPushButton*>("toggleServerLog")->click();
        require(services->findChild<QWidget*>("serverLog")->isVisible(), "Server log inaccessible");
        details->grab().save("build-qtcreator/dialog-services-preview.png");
        sidebar->findChild<QToolButton*>("collapseSidebar")->click();
        require(!sidebar->isVisible() && !details->isVisible(), "Collapse left an orphan service popup");
        page->findChild<QToolButton*>("toggleSidebar")->click();
        require(sidebar->isVisible(), "Cannot restore sidebar");
        for (auto* button : sidebar->findChildren<QPushButton*>()) {
            if (button->text() != QStringLiteral("Документы чата")) continue;
            button->click(); button->click();
            require(button->isChecked(), "Clicking the active documents page cleared its selection");
        }
        sidebar->setChatPageActive(true);
        services->setEmbeddingStatus(QStringLiteral("Ошибка: сервер недоступен"));
        require(summary->text().contains(QStringLiteral("Ошибка")), "Collapsed summary hides service error");
        services->setEmbeddingStatus(QStringLiteral("Готов · CUDA"));
        const auto a = chats.currentChat()->id;
        const auto b = chats.createChat();
        waitFor([&]() { return service.isChatReady(b) && !chats.isBusy(); });
        const auto c = chats.createChat();
        waitFor([&]() { return service.isChatReady(c) && !chats.isBusy(); });
        chats.selectChat(b);
        waitFor([&]() { return service.isChatReady(b); });
        require(chats.deleteChat(a), "Delete idle background chat rejected");
        require(!chats.canDeleteChat(a) && !chats.selectChat(a), "Deleting chat still selectable");
        require(chats.selectChat(c), "Navigation blocked while another chat is deleted");
        waitFor([&]() { return !chats.isBusy() && service.isChatReady(c); });
        require(chats.currentChat()->id == c && model.rowCount() == 2, "Deletion changed another chat's selection");
        require(request(service.storage(), "bootstrap", {}).value("currentChatId").toString() == c, "Stored selection lost navigation");

        // Seed one ready document, then verify deletion clears the visible workspace.
        const auto docId = QUuid::createUuid().toString().mid(1,36);
        const auto source = QDir(data.path()).filePath("original.txt");
        { QFile file(source); require(file.open(QIODevice::WriteOnly), "Fixture creation failed"); file.write("Document text"); }
        QJsonArray vector; for (int i = 0; i < 768; ++i) vector.append(i == 0 ? 1.0 : 0.0);
        const QJsonObject doc{{"id",docId},{"sourcePath",source},{"text","Document text"},{"warnings",QJsonArray{}},
            {"blocks",QJsonArray{QJsonObject{{"type","text"},{"text","Document text"},{"pageNumber",0}}}}};
        const QJsonObject chunk{{"documentId",docId},{"text","Document text"},{"sourcePath",source},{"startOffset",0}};
        request(service.storage(), "save_document", {{"chatId",c},{"document",doc},
            {"entries",QJsonArray{QJsonObject{{"chunk",chunk},{"embedding",vector}}}}});
        bool loaded = false;
        service.activateChat(c, [&](const QString& error) { failure = error; loaded = true; });
        waitFor([&]() { return loaded; });
        require(failure.isEmpty() && service.documentManager()->documentCount() == 1, "Document seed not restored");
        // Publish through the real UI, then remove its owner chat.
        QPointer<DocumentLibraryDialog> library=new DocumentLibraryDialog(&service,c,docId,&window);
        library->show();
        auto* libraryList=library->findChild<QListWidget*>("libraryDocuments");
        waitFor([&](){return libraryList->count()==1 && libraryList->isEnabled();});
        const auto libraryId=libraryList->item(0)->data(Qt::UserRole).toString();
        library->grab().save("build-qtcreator/library-preview.png");
        std::fprintf(stderr,"Library: publication passed\n");
        library->close(); waitFor([&](){return !library;});
        std::fprintf(stderr,"Library: first close passed\n");
        require(chats.deleteChat(c), "Current chat deletion rejected");
        waitFor([&]() { return !chats.isBusy() && service.isChatReady(b); });
        require(chats.currentChat()->id == b && model.rowCount() == 1 &&
                service.documentManager()->documentCount() == 0 && QFile::exists(source), "Chat/document deletion corrupted workspace or source");

        require(QFile::remove(source), "Cannot remove original library fixture");
        std::fprintf(stderr,"Library: source deletion passed\n");
        library=new DocumentLibraryDialog(&service,b,{},&window); library->show();
        libraryList=library->findChild<QListWidget*>("libraryDocuments");
        auto* librarySearch=library->findChild<QLineEdit*>("librarySearch");
        auto* libraryAdd=library->findChild<QPushButton*>("attachLibraryDocuments");
        waitFor([&](){return libraryList->count()==1 && libraryList->isEnabled();});
        librarySearch->setText("no-such-file"); waitFor([&](){return libraryList->count()==0 && librarySearch->isEnabled();});
        librarySearch->clear(); waitFor([&](){return libraryList->count()==1 && librarySearch->isEnabled();});
        std::fprintf(stderr,"Library: search passed\n");
        libraryList->item(0)->setSelected(true); require(libraryAdd->isEnabled(), "Library attach unavailable with models stopped");
        libraryAdd->click();
        waitFor([&](){return !service.isBusy() && service.documentManager()->documentCount()==1 && libraryList->isEnabled();});
        require(service.documentManager()->documentAt(0)->text=="Document text", "Saved library content not restored");
        require(service.documentManager()->documentAt(0)->id!=docId && service.documentManager()->documentAt(0)->id!=libraryId, "Library copy reused another owner's ID");
        std::fprintf(stderr,"Library: attachment passed\n");
        // Closing during an async request must not leave callbacks pointing at deleted widgets.
        libraryAdd->click(); library->close(); waitFor([&](){return !library && !service.isBusy();});
        require(service.documentManager()->documentCount()==1, "Reattaching duplicated the document");
        std::fprintf(stderr,"Library: closing during attach passed\n");
        library=new DocumentLibraryDialog(&service,b,{},&window); library->show();
        libraryList=library->findChild<QListWidget*>("libraryDocuments");
        waitFor([&](){return libraryList->count()==1 && libraryList->isEnabled();}); libraryList->item(0)->setSelected(true);
        std::fprintf(stderr,"Library: opening delete confirmation\n");
        library->findChild<QPushButton*>("deleteLibraryDocument")->click();
        QPointer<QDialog> confirmation=library->findChild<QDialog*>("removeLibraryConfirmation");
        require(confirmation && confirmation->findChild<QPushButton*>("confirmLibraryDeletion"),"Library deletion lacks confirmation");
        confirmation->reject(); waitFor([&](){return !confirmation && libraryList->isEnabled();});
        require(libraryList->count()==1,"Cancelling library deletion removed a document");
        library->findChild<QPushButton*>("deleteLibraryDocument")->click();
        confirmation=library->findChild<QDialog*>("removeLibraryConfirmation");
        confirmation->findChild<QPushButton*>("confirmLibraryDeletion")->click();
        waitFor([&](){return libraryList->count()==0 && libraryList->isEnabled();});
        require(service.documentManager()->documentCount()==1, "Deleting the library removed a chat copy");
        library->close(); waitFor([&](){return !library;});
        std::fprintf(stderr,"Library: deletion and cancellation passed\n");
        // Later source-panel fixtures reuse this original path.
        { QFile file(source); require(file.open(QIODevice::WriteOnly),"Cannot restore source fixture"); file.write("Document text"); }

        // A pending DB question must reject deletion without removing a row in Qt.
        const auto question = QUuid::createUuid().toString().mid(1,36);
        request(service.storage(), "save_question", {{"id",question},{"chatId",b},{"text","Question"},{"title","Question"}});
        chats.deleteChat(b);
        waitFor([&]() { return !chats.isBusy(); });
        require(!failure.isEmpty() && model.rowCount() == 1 && chats.currentChat()->id == b, "Failed deletion removed UI state");
        failure.clear();
        request(service.storage(), "finish_question", {{"questionId",question},{"error","Fixture cancelled"}});

        ChatMessage user; user.role = MessageRole::User; user.status = "pending";
        user.text = QStringLiteral("Сравни требования из двух документов и выдели основные отличия.");
        page->setMessages({user}); page->setWaitingForAnswer(true);
        // ThinkingIndicator has no Q_OBJECT; locate it by its objectName instead.
        auto* bubble = page->findChild<QWidget*>("thinkingIndicator");
        require(bubble && bubble->isVisible(), "Waiting indicator missing");
        auto* timer = bubble->findChild<QTimer*>();
        require(timer && timer->isActive(), "Animation not started");
        const auto before = bubble->grab().toImage();
        QElapsedTimer animation; animation.start(); waitFor([&]() { return animation.elapsed() >= 120; });
        require(before != bubble->grab().toImage(), "Spinner did not move");
        page->hide(); require(!timer->isActive(), "Hidden chat keeps animating");
        page->show(); require(timer->isActive(), "Spinner did not resume");
        page->setMessages({user}); require(page->findChild<QWidget*>("thinkingIndicator") == bubble, "Refresh replaced activity widget");
        QCoreApplication::processEvents();
        window.grab().save("build-qtcreator/chat-waiting-preview.png");

        const auto photo = QDir::current().filePath("test-documents/images/standalone-plate.png");
        const auto draftFile = QDir::current().filePath("README.md");
        page->setAttachmentPaths({photo, draftFile});
        auto* draftAttachments = page->findChild<QWidget*>("draftAttachments");
        require(draftAttachments && draftAttachments->isVisible() && draftAttachments->findChildren<QWidget*>("attachmentCard").size() == 2,
                "Composer attachments missing");
        user.attachments = {{"standalone-plate.png",photo,"saved-document",photo}};
        user.text = QStringLiteral("Какое давление указано здесь?");
        page->setMessages({user});
        QElapsedTimer attachmentLayout; attachmentLayout.start();
        waitFor([&]() { return attachmentLayout.elapsed() >= 150; });
        window.grab().save("build-qtcreator/chat-attachments-preview.png");
        bool removed = false;
        for (auto* button : draftAttachments->findChildren<QToolButton*>()) if (button->toolTip() == QStringLiteral("Убрать вложение")) {
            button->click(); removed = true; break;
        }
        require(removed, "Cannot remove draft attachment");
        waitFor([&]() { return page->attachmentPaths().size() == 1; });
        page->clearInput(); require(page->attachmentPaths().isEmpty() && !draftAttachments->isVisible(), "Sending retained draft attachments");
        page->setAttachmentPaths({photo, draftFile});
        for (auto* button : draftAttachments->findChildren<QToolButton*>()) if (button->toolTip() == QStringLiteral("Убрать вложение")) {
            button->click(); break;
        }
        page->setAttachmentPaths({photo}); // Another chat was selected before the queued click completed.
        QCoreApplication::processEvents();
        require(page->attachmentPaths() == QStringList{photo}, "Queued removal changed another chat's draft");
        page->clearInput();

        // Exercise QTextEdit's real paste path, with isolated clipboard image storage.
        QStandardPaths::setTestModeEnabled(true);
        const auto oldApplicationName = app.applicationName();
        app.setApplicationName("clipboard-test-" + QUuid::createUuid().toString().mid(1,36));
        auto* input = page->findChild<QTextEdit*>("messageInput");
        auto* composer = page->findChild<MessageComposer*>();
        require(input && composer, "Composer input missing");
        QString pasteError;
        const auto pasteErrorConnection = QObject::connect(composer, &MessageComposer::attachmentError,
            [&](const QString& error) { pasteError = error; });
        QImage screenshot(480, 240, QImage::Format_ARGB32);
        screenshot.fill(QColor("#28292B"));
        { QPainter painter(&screenshot); painter.setPen(Qt::white);
          painter.drawText(screenshot.rect(), Qt::AlignCenter, QStringLiteral("Ошибка подключения: порт занят")); }
        page->setInputText(QStringLiteral("Как это исправить?"));
        QApplication::clipboard()->setImage(screenshot);
        require(input->canPaste(), "Image-only clipboard does not enable Paste");
        input->setFocus();
        QKeyEvent pasteKey(QEvent::KeyPress, Qt::Key_V, Qt::ControlModifier);
        QApplication::sendEvent(input, &pasteKey);
        require(pasteError.isEmpty() && page->attachmentPaths().size() == 1, "Ctrl+V did not attach screenshot");
        const auto pastedPath = page->attachmentPaths().front();
        require(QImage(pastedPath) == screenshot, "Pasted screenshot lost image data");
        require(page->inputText() == QStringLiteral("Как это исправить?"), "Image paste replaced question text");
        require(draftAttachments->isVisible() && draftAttachments->findChildren<QWidget*>("attachmentCard").size() == 1,
                "Pasted screenshot has no attachment preview");
        QCoreApplication::processEvents();
        window.grab().save("build-qtcreator/chat-clipboard-preview.png");
        input->paste();
        require(page->attachmentPaths().size() == 1, "Repeated paste duplicated identical attachment");
        page->setAttachmentPaths({}); // Switch to another chat's draft, then return.
        require(QFile::exists(pastedPath), "Switching chats deleted pasted source");
        page->setAttachmentPaths({pastedPath});
        QApplication::clipboard()->setPixmap(QPixmap::fromImage(screenshot));
        input->paste();
        require(pasteError.isEmpty(), qPrintable(pasteError));
        require(page->attachmentPaths() == QStringList{pastedPath}, "Pixmap paste did not reuse identical screenshot");
        page->setAttachmentPaths({photo, draftFile, source});
        input->paste();
        require(page->attachmentPaths().size() == 4, "Screenshot cannot be combined with selected files");
        input->paste();
        require(!pasteError.isEmpty() && page->attachmentPaths().size() == 4, "Paste bypassed attachment limit");
        page->addAttachmentPaths({"fifth.txt"});
        require(page->attachmentPaths().size() == 4, "File selection bypassed shared attachment limit");
        page->clearInput();
        page->setUploadEnabled(false); pasteError.clear(); input->paste();
        require(!pasteError.isEmpty() && page->attachmentPaths().isEmpty(), "Paste bypassed disabled uploads");
        // Text, including rich clipboard text, must still paste while uploads are disabled.
        auto* textMime = new QMimeData;
        textMime->setHtml("<b>Plain question</b>"); textMime->setText("Plain question");
        QApplication::clipboard()->setMimeData(textMime);
        input->paste();
        require(page->inputText() == "Plain question" && page->attachmentPaths().isEmpty(), "Text paste broken");
        page->setUploadEnabled(true); pasteError.clear();
        auto* invalidImage = new QMimeData; invalidImage->setImageData(QImage());
        QApplication::clipboard()->setMimeData(invalidImage); input->paste();
        require(!pasteError.isEmpty() && page->attachmentPaths().isEmpty() && page->inputText() == "Plain question",
                "Invalid image silently replaced draft");
        page->setAttachmentPaths({pastedPath});
        for (auto* button : draftAttachments->findChildren<QToolButton*>()) if (button->toolTip() == QStringLiteral("Убрать вложение")) {
            button->click(); break;
        }
        waitFor([&]() { return page->attachmentPaths().isEmpty(); });
        page->setAttachmentPaths({pastedPath});
        page->clearInput();
        require(QFile::exists(pastedPath), "Clearing sent draft removed source before indexing");
        QObject::disconnect(pasteErrorConnection);
        QFile::remove(pastedPath);
        QDir().rmdir(QFileInfo(pastedPath).absolutePath());
        QDir().rmdir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
        QStandardPaths::setTestModeEnabled(false);
        app.setApplicationName(oldApplicationName);
        QApplication::clipboard()->clear();

        int submitted = 0;
        QString submittedText;
        QStringList submittedAttachments;
        const auto submitConnection = QObject::connect(page, &ChatWidget::sendRequested, [&](const QString& text) {
            ++submitted; submittedText = text; submittedAttachments = page->attachmentPaths();
        });
        const auto pressEnter = [&](int key, Qt::KeyboardModifiers modifiers, bool repeat = false) {
            QKeyEvent event(QEvent::KeyPress, key, modifiers, QString(), repeat);
            QApplication::sendEvent(input, &event);
        };
        page->setSendEnabled(true);
        page->setInputText(QStringLiteral("Первая строка"));
        input->moveCursor(QTextCursor::End);
        pressEnter(Qt::Key_Return, Qt::ShiftModifier);
        require(submitted == 0 && page->inputText() == QStringLiteral("Первая строка\n"), "Shift+Enter sent message or lost newline");
        input->insertPlainText(QStringLiteral("Вторая строка"));
        pressEnter(Qt::Key_Return, Qt::NoModifier);
        require(submitted == 1 && submittedText == QStringLiteral("Первая строка\nВторая строка")
                && page->inputText() == submittedText, "Enter did not submit exact multiline question");
        pressEnter(Qt::Key_Return, Qt::NoModifier, true);
        require(submitted == 1, "Held Enter repeatedly submitted message");
        page->setSendEnabled(false);
        pressEnter(Qt::Key_Return, Qt::NoModifier);
        require(submitted == 1 && page->inputText() == submittedText, "Enter bypassed disabled send button");
        page->setSendEnabled(true); page->clearInput(); page->setAttachmentPaths({photo});
        pressEnter(Qt::Key_Enter, Qt::KeypadModifier);
        require(submitted == 2 && submittedText.isEmpty() && submittedAttachments == QStringList{photo},
                "Numpad Enter did not submit image-only question");
        pressEnter(Qt::Key_Enter, Qt::KeypadModifier | Qt::ShiftModifier);
        require(submitted == 2 && page->inputText() == "\n", "Shift+Numpad Enter did not insert newline");
        QObject::disconnect(submitConnection);
        page->clearInput();

        waitFor([&]() { return page->findChild<QWidget*>("messageComposer")->height() <= 100; });
        const QString partial = QStringLiteral("**План проверки**\n\n1. Чтение документа.\n2. Поиск подходящих фрагментов.\n\n");
        page->setStreamingAnswer(partial);
        auto* streaming = page->findChild<QWidget*>("streamingAnswer");
        require(streaming, "Streaming bubble missing");
        page->setStreamingAnswer(partial + QStringLiteral("3. Формирование ответа.\n"));
        require(page->findChild<QWidget*>("streamingAnswer") == streaming, "Each update recreated the message widget");
        auto* copy = streaming->findChild<QToolButton*>("copyAnswer");
        require(copy, "Copy answer action missing");
        copy->click(); require(QApplication::clipboard()->text().contains(QStringLiteral("Формирование ответа")), "Copy lost streamed text");
        QCoreApplication::processEvents();
        window.grab().save("build-qtcreator/chat-streaming-preview.png");
        page->setStreamingAnswer(partial.repeated(25));
        auto* scroll = page->findChild<QScrollArea*>()->verticalScrollBar();
        waitFor([&]() { return scroll->maximum() > 1000; });
        require(scroll->maximum() - scroll->value() <= 24, "Streaming no longer follows bottom");
        auto* streamText = streaming->findChild<QTextBrowser*>("answerText");
        require(streamText, "Streaming text view missing");
        const int beforeWheel = scroll->value();
        const QPoint wheelPosition(30, 30);
        QWheelEvent wheel(wheelPosition, streamText->viewport()->mapToGlobal(wheelPosition),
                          QPoint(), QPoint(0, 120), 120, Qt::Vertical, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(streamText->viewport(), &wheel);
        require(scroll->value() < beforeWheel, "Mouse wheel over an answer no longer scrolls the chat");
        scroll->setValue(100);
        page->setStreamingAnswer(partial.repeated(30));
        QCoreApplication::processEvents();
        require(scroll->value() == 100, "Streaming pulled reader away from older text");
        ChatMessage completed; completed.role = MessageRole::Assistant; completed.text = partial.repeated(30);
        page->setMessages({user, completed}, false); page->setStreamingAnswer({});
        QCoreApplication::processEvents();
        require(!page->findChild<QWidget*>("streamingAnswer") && scroll->value() == 100, "Final answer duplicated preview or reset scroll");
        page->setMessages({user}); page->setStreamingAnswer(partial);
        page->setMessages({});
        require(!page->findChild<QWidget*>("streamingAnswer"), "Switching chats retained another chat's preview");
        page->setMessages({user}); page->setStreamingAnswer(partial);
        require(page->findChild<QWidget*>("streamingAnswer"), "Returning to a streaming chat lost preview");
        page->setMessages({user});

        // A long code line/table must scroll inside the answer, never the whole chat.
        user.attachments = {{"protocol.docx", "protocol.docx", {}, {}}, {"manual.pdf", "manual.pdf", {}, {}}};
        ChatMessage wide; wide.role = MessageRole::Assistant;
        const QString codeLine = "    auto veryLongIdentifier = " + QString("value_").repeated(80) + ";";
        wide.text = "## Code\n\n```cpp\n" + codeLine + "\nreturn 0;\n```\n\n";
        wide.text += "| Field | Value |\n| --- | --- |\n| pressure | 7.5 bar |\n\n";
        const QString columns = "| Heading ";
        wide.text += columns.repeated(18) + "|\n" + QString("| --- ").repeated(18) + "|\n"
            + QString("| table value ").repeated(18) + "|\n\nEnd of answer.";
        page->setMessages({user, wide}); page->setWaitingForAnswer(true);
        auto* messages = page->findChild<QScrollArea*>();
        QPointer<QTextBrowser> answer = page->findChild<QTextBrowser*>("answerText");
        require(answer, "Rich answer view missing");
        auto settleLayout = [&]() {
            QElapsedTimer settling; settling.start();
            waitFor([&]() { return settling.elapsed() >= 100; });
            require(messages->horizontalScrollBar()->maximum() == 0, "Wide content stretched the whole chat");
            require(messages->widget()->width() <= messages->viewport()->width(), "Message column wider than viewport");
            if (auto* currentAnswer = page->findChild<QTextBrowser*>("answerText"))
                require(currentAnswer->verticalScrollBar()->maximum() == 0, "Answer height clips document text");
            if (answer) require(answer->toPlainText().contains(codeLine), "Wrapping changed the code contents");
        };
        window.resize(1920, 1000); settleLayout();
        window.resize(1100, 680); settleLayout();
        require(answer->horizontalScrollBar()->maximum() > 0, "Wide code/table cannot be scrolled");
        window.grab().save("build-qtcreator/chat-wide-content-preview.png");
        QPushButton* sources = nullptr;
        for (auto* button : page->findChildren<QPushButton*>())
            if (button->text() == QStringLiteral("Источники")) sources = button;
        require(sources, "Sources button missing");
        sources->click(); settleLayout(); sources->click(); settleLayout();
        page->setMessages({user});

        bool menuClicked = false, menuEnabled = false;
        auto* list = sidebar->findChild<QListView*>();
        QTimer::singleShot(20, [&]() {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!menu || menu->actions().isEmpty()) return;
            auto* action = menu->actions().first(); menuEnabled = action->isEnabled();
            auto preview = window.grab();
            { QPainter painter(&preview); painter.drawPixmap(130, 245, menu->grab()); }
            preview.save("build-qtcreator/chat-menu-preview.png");
            const auto point = menu->actionGeometry(action).center();
            QMouseEvent press(QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(menu, &press); QApplication::sendEvent(menu, &release); menuClicked = true;
        });
        QMetaObject::invokeMethod(list, "customContextMenuRequested", Qt::DirectConnection,
            Q_ARG(QPoint, list->visualRect(model.index(0,0)).center()));
        require(menuClicked && menuEnabled, "Delete context menu inaccessible");
        waitFor([&]() { return !chats.isBusy() && chats.currentChat() && chats.currentChat()->id != b &&
                              service.isChatReady(chats.currentChat()->id); });
        require(model.rowCount() == 1 && chats.currentChat()->messages.empty(), "Last chat was not replaced");
        page->setWaitingForAnswer(false);
        require(!timer->isActive() && !bubble->isVisible(), "Completed answer left activity running");
        const auto restored = request(service.storage(), "bootstrap", {});
        require(restored.value("chats").toArray().size() == 1 && failure.isEmpty(), "Deleted chats restored or UI operation failed");
        // Restore source metadata through the real worker/ChatManager path after restart.
        const auto sourceChat = chats.currentChat()->id;
        request(service.storage(), "save_document", {{"chatId",sourceChat},{"document",doc},
            {"entries",QJsonArray{QJsonObject{{"chunk",chunk},{"embedding",vector}}}}});
        const auto q1 = QUuid::createUuid().toString().mid(1,36), a1 = QUuid::createUuid().toString().mid(1,36);
        const auto q2 = QUuid::createUuid().toString().mid(1,36), a2 = QUuid::createUuid().toString().mid(1,36);
        const QString excerpt = QStringLiteral("## Таблица 5.4.1\n\n| Параметр | Значение |\n|---|---|\n| Давление | 7.5 bar |\n| Кабинет | 218 |\n\nСохранённый фрагмент ORIGINAL-7319.");
        request(service.storage(), "save_question", {{"id",q1},{"chatId",sourceChat},{"text",QStringLiteral("Какое давление указано в таблице?")},{"title","Sources"}});
        request(service.storage(), "save_context", {{"id",q1},{"context",excerpt},{"references",QJsonArray{QJsonObject{
            {"documentId",docId},{"text",excerpt},{"pageFirst",7},{"pageLast",8},{"tableIndices",QJsonArray{0}},
            {"blockFirst",0},{"blockLast",1}}}}});
        request(service.storage(), "finish_question", {{"questionId",q1},{"id",a1},{"text",QStringLiteral("В таблице указано **7.5 bar**. Источник [1].")}});
        request(service.storage(), "save_question", {{"id",q2},{"chatId",sourceChat},{"text",QStringLiteral("Вопрос из старой версии")},{"title","Sources"}});
        request(service.storage(), "save_context", {{"id",q2},{"context","LEGACY-ONLY"}});
        request(service.storage(), "finish_question", {{"questionId",q2},{"id",a2},{"text","Legacy answer"}});
        service.storage()->stop(); waitFor([&]() { return service.storage()->isStopped(); });
        service.storage()->start();
        waitFor([&]() { return service.storage()->isReady() && !chats.isBusy() && chats.currentChat() && chats.currentChat()->messages.size() == 4; });
        const auto savedMessages = chats.currentChat()->messages;
        page->showNotification({});
        page->setChatTitle(QStringLiteral("Проверка источников"));
        require(savedMessages[1].replyTo == q1 && savedMessages[0].sourceReferences.size() == 1, "Restored answer lost source identity");
        page->setMessages(savedMessages); settleLayout();
        auto sourceButtons = page->findChildren<QToolButton*>("messageSources");
        require(sourceButtons.size() == 2 && sourceButtons[0]->isVisible(), "Per-answer source buttons missing");
        sourceButtons[0]->click(); settleLayout();
        auto* sourceList = page->findChild<QListWidget*>("answerSources");
        require(sourceList->count() == 1 && sourceList->item(0)->text().contains(QStringLiteral("7–8")), "Wrong answer sources or PDF page range");
        page->findChild<QPushButton*>("openSourceFragment")->click(); settleLayout();
        QPointer<QDialog> preview = page->findChild<QDialog*>("sourcePreview");
        require(preview && preview->isVisible(), "Source fragment did not open");
        auto* excerptView = preview->findChild<QTextBrowser*>("sourceExcerpt");
        require(excerptView->toPlainText().contains("ORIGINAL-7319") && !excerptView->toPlainText().contains("LEGACY-ONLY"), "Answer displayed another question's evidence");
        bool renderedTable = false;
        for (auto* frame : excerptView->document()->rootFrame()->childFrames()) if (qobject_cast<QTextTable*>(frame)) renderedTable = true;
        require(renderedTable, "Source table lost its columns");
        require(preview->findChild<QPushButton*>("openSourceOriginal")->isEnabled(), "Existing source file unavailable");
        window.grab().save("build-qtcreator/chat-sources-preview.png");
        preview->grab().save("build-qtcreator/source-fragment-preview.png");
        preview->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        // Missing originals do not erase the evidence from the answer.
        require(QFile::remove(source), "Cannot remove isolated original fixture");
        page->findChild<QPushButton*>("openSourceFragment")->click(); settleLayout();
        preview = page->findChild<QDialog*>("sourcePreview");
        require(preview && !preview->findChild<QPushButton*>("openSourceOriginal")->isEnabled() &&
                preview->findChild<QTextBrowser*>("sourceExcerpt")->toPlainText().contains("ORIGINAL-7319"), "Missing file destroyed snapshot");
        sourceButtons[1]->click(); settleLayout();
        require((!preview || !preview->isVisible()) && sourceList->count() == 0 &&
                page->findChild<QTextEdit*>("sourceText")->toPlainText() == "LEGACY-ONLY", "Legacy snapshot or selection changed incorrectly");
        page->setMessages({}); settleLayout();
        require(sourceList->count() == 0 && page->findChild<QTextEdit*>("sourceText")->toPlainText().isEmpty(), "Sources leaked into another chat");
        auto imageQuestion = savedMessages[0];
        auto imageSource = imageQuestion.sourceReferences[0].toObject();
        imageSource["pageFirst"] = 0; imageSource["pageLast"] = 0;
        imageSource["imagePath"] = photo; imageSource["sourcePath"] = "//server/share/picture.png";
        imageSource["text"] = QStringLiteral("Сохранённое описание: PUMP-417, 7.5 bar.\n\n![untrusted](file:///C:/Windows/win.ini)");
        imageQuestion.sourceReferences = QJsonArray{imageSource};
        page->setMessages({imageQuestion, savedMessages[1]}); settleLayout();
        page->findChild<QToolButton*>("messageSources")->click();
        page->findChild<QPushButton*>("openSourceFragment")->click(); settleLayout();
        preview = page->findChild<QDialog*>("sourcePreview");
        auto* imageLabel = preview->findChild<QLabel*>("sourceImage");
        require(imageLabel && imageLabel->pixmap() && !imageLabel->pixmap()->isNull(), "Source image preview missing");
        require(preview->findChild<QLabel*>("sourcePosition")->text().contains(QStringLiteral("Страница не определена")), "Unknown page was invented");
        require(!preview->findChild<QPushButton*>("openSourceOriginal")->isEnabled(), "Network path could be opened as a local source");
        require(preview->findChild<QTextBrowser*>("sourceExcerpt")->document()->resource(QTextDocument::ImageResource,
                QUrl::fromLocalFile(photo)).isNull(), "Untrusted Markdown loaded a local resource");
        preview->grab().save("build-qtcreator/source-image-preview.png");
        page->setMessages({}); settleLayout();
        // Compare real stored revisions through the worker, independently of model servers.
        QStringList revisionIds;
        for (int revision = 0; revision < 2; ++revision) {
            const auto id = QUuid::createUuid().toString().mid(1,36); revisionIds.append(id);
            const auto path = data.path() + QString("/protocol-v%1.docx").arg(revision + 1);
            QFile original(path); require(original.open(QIODevice::WriteOnly), "Cannot create revision fixture");
            original.write("Stored extraction fixture"); original.close();
            const QString pressure = revision ? "8.0 bar" : "7.5 bar";
            QJsonArray rows{QJsonArray{QStringLiteral("Поле"),QStringLiteral("Значение")},
                QJsonArray{QStringLiteral("Давление"),pressure}, QJsonArray{QStringLiteral("Скорость"),revision ? "19200" : "9600"}};
            if (revision) rows.append(QJsonArray{QStringLiteral("Контроль"),"CRC-16"});
            QJsonObject revisionDoc{{"id",id},{"sourcePath",path},{"text",pressure},{"warnings",QJsonArray{}},
                {"blocks",QJsonArray{QJsonObject{{"type","table"},{"tableRows",rows},{"pageNumber",revision ? 8 : 7},{"tableIndex",0}}}}};
            QJsonObject revisionChunk{{"documentId",id},{"text",pressure},{"startOffset",0}};
            request(service.storage(), "save_document", {{"chatId",sourceChat},{"document",revisionDoc},
                {"entries",QJsonArray{QJsonObject{{"chunk",revisionChunk},{"embedding",vector}}}}});
        }
        QPointer<DocumentComparisonDialog> comparison = new DocumentComparisonDialog(service.storage(), sourceChat, revisionIds, &window);
        comparison->show();
        auto* changes = comparison->findChild<QTableWidget*>("comparisonChanges");
        auto* swap = comparison->findChild<QPushButton*>("comparisonSwap");
        waitFor([&]() { return swap->isEnabled(); });
        require(changes->rowCount() == 3 && comparison->findChild<QLabel*>("comparisonSummary")->text().contains(QStringLiteral("Изменено: 2")), "Wrong revision diff");
        changes->selectRow(0);
        comparison->findChild<QPushButton*>("comparisonBefore")->click();
        preview = comparison->findChild<QDialog*>("sourcePreview");
        require(preview && preview->findChild<QTextBrowser*>("sourceExcerpt")->toPlainText().contains("7.5 bar"), "Old fragment lost table value");
        require(preview->findChild<QLabel*>("sourcePosition")->text().contains(QStringLiteral("Страница 7")), "Old source has new page");
        preview->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        comparison->findChild<QPushButton*>("comparisonAfter")->click();
        preview = comparison->findChild<QDialog*>("sourcePreview");
        require(preview && preview->findChild<QTextBrowser*>("sourceExcerpt")->toPlainText().contains("8.0 bar"), "New fragment has old value");
        preview->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        changes->selectRow(2);
        require(!comparison->findChild<QPushButton*>("comparisonBefore")->isEnabled(), "Added row invented an old source");
        comparison->grab().save("build-qtcreator/document-comparison-preview.png");
        swap->click();
        require(comparison->findChild<QLabel*>("comparisonFiles")->text().startsWith(QStringLiteral("Было: protocol-v2.docx")), "Swap did not change file labels");
        require(comparison->findChild<QLabel*>("comparisonSummary")->text().contains(QStringLiteral("Удалено: 1")), "Swap did not invert counts");
        comparison->resize(860,500); QCoreApplication::processEvents();
        require(changes->horizontalScrollBar()->maximum() == 0, "Comparison table breaks minimum layout");
        comparison->grab().save("build-qtcreator/document-comparison-compact-preview.png");
        comparison->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        // Closing before the asynchronous request completes must not touch deleted widgets.
        comparison = new DocumentComparisonDialog(service.storage(), sourceChat, revisionIds, &window);
        comparison->show(); QCoreApplication::processEvents(); comparison->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        require(!comparison, "Closed comparison was not destroyed");
        request(service.storage(), "bootstrap", {});
        // Cross-chat failure is rendered inline and never produces a report from another chat.
        comparison = new DocumentComparisonDialog(service.storage(), QUuid::createUuid().toString().mid(1,36), revisionIds, &window);
        comparison->show();
        waitFor([&]() { return comparison->findChild<QLabel*>("comparisonSummary")->text().contains(QStringLiteral("Не удалось")); });
        require(!comparison->findChild<QPushButton*>("comparisonSwap")->isEnabled(), "Failed comparison allowed swap");
        comparison->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QPointer<DocumentTablesDialog> tableDialog = new DocumentTablesDialog(service.storage(), sourceChat, revisionIds.first(), &window);
        tableDialog->show();
        auto* tableSelector = tableDialog->findChild<QComboBox*>("storedTables");
        auto* exportCsv = tableDialog->findChild<QPushButton*>("exportTableCsv");
        waitFor([&]() { return exportCsv->isEnabled(); });
        auto* cells = tableDialog->findChild<QTableView*>("tableCells");
        require(tableSelector->count() == 1 && cells->model()->rowCount() == 3, "Stored table rows missing");
        require(cells->model()->index(1,1).data().toString() == "7.5 bar", "Table preview has another revision");
        tableDialog->findChild<QPushButton*>("tableSource")->click();
        preview = tableDialog->findChild<QDialog*>("sourcePreview");
        require(preview && preview->findChild<QTextBrowser*>("sourceExcerpt")->toPlainText().contains("7.5 bar"), "Table source missing");
        preview->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        tableDialog->grab().save("build-qtcreator/document-tables-preview.png");
        // Exercise the actual Save dialog and atomic file writer, not a mock slot.
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        const auto exportedCsv = data.path() + "/exported-table.csv";
        exportCsv->click();
        auto* chooser = tableDialog->findChild<QFileDialog*>();
        require(chooser, "Export did not offer file selection");
        chooser->selectFile(exportedCsv);
        QMetaObject::invokeMethod(chooser, "accept", Qt::DirectConnection);
        waitFor([&]() { return tableDialog->findChild<QLabel*>("tableStatus")->text().contains(QStringLiteral("Сохранено:")); });
        QFile exported(exportedCsv); require(exported.open(QIODevice::ReadOnly), "Export was not written");
        const auto csvData = exported.readAll(); exported.close();
        require(csvData.startsWith("\xef\xbb\xbf") && csvData.contains("7.5 bar"), "CSV encoding or values wrong");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        const auto exportedXlsx = data.path() + "/exported-table.xlsx";
        tableDialog->findChild<QPushButton*>("exportTableXlsx")->click();
        chooser = tableDialog->findChild<QFileDialog*>(); chooser->selectFile(exportedXlsx);
        QMetaObject::invokeMethod(chooser, "accept", Qt::DirectConnection);
        waitFor([&]() { return tableDialog->findChild<QLabel*>("tableStatus")->text().contains(exportedXlsx); });
        QFile xlsx(exportedXlsx); require(xlsx.open(QIODevice::ReadOnly) && xlsx.read(2) == "PK", "XLSX is not a workbook ZIP"); xlsx.close();
        QString exportFailure;
        require(!saveTableExport(exportedCsv, {{"data","corrupt!"}}, &exportFailure), "Malformed export was saved");
        require(exported.open(QIODevice::ReadOnly) && exported.readAll() == csvData, "Failed export overwrote an existing file"); exported.close();
        tableDialog->resize(820,520); QCoreApplication::processEvents();
        tableDialog->grab().save("build-qtcreator/document-tables-compact-preview.png");
        tableDialog->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        tableDialog = new DocumentTablesDialog(service.storage(), sourceChat, docId, &window);
        tableDialog->show();
        waitFor([&]() { return tableDialog->findChild<QLabel*>("tableStatus")->text().contains(QStringLiteral("Готовых таблиц")); });
        require(!tableDialog->findChild<QPushButton*>("exportTableCsv")->isEnabled(), "Empty table list can be exported");
        tableDialog->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        tableDialog = new DocumentTablesDialog(service.storage(), sourceChat, revisionIds.first(), &window);
        tableDialog->show(); QCoreApplication::processEvents(); tableDialog->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        require(!tableDialog, "Table dialog survived close during request");
        request(service.storage(), "bootstrap", {});
        service.storage()->stop(); waitFor([&]() { return service.storage()->isStopped(); });
        qInfo() << "Qt table preview, CSV/XLSX saving, revision comparison, sources, restart, deletion, navigation, attachments, streaming and layout passed";
        return 0;
    } catch (const std::exception& error) {
        qCritical() << error.what(); service.storage()->stop();
        waitFor([&]() { return service.storage()->isStopped(); }); return 1;
    }
}
