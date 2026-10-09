// Real configured llama servers; a private test database and a caller-supplied DOCX/PDF.
#include "application/assistant_service.hpp"
#include "application/document_import_queue.hpp"
#include "chat/chat_manager.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QTimer>
#include <QElapsedTimer>
#include <QDebug>
#include <cstdio>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv); app.setApplicationName("ServiceImportTests");
    if (app.arguments().size() < 2) return 2;
    QDir().mkpath("build-import-queue");
    QTemporaryDir data(QDir::current().filePath("build-import-queue/service-XXXXXX"));
    if (!data.isValid()) return 2;
    qputenv("LOCAL_ASSISTANT_STORAGE_DIR", data.path().toLocal8Bit());
    qputenv("LOCAL_ASSISTANT_STORAGE_PORT", "55436");
    AssistantService service; ChatManager chats(&service);
    int result = 1;
    bool enqueued = false, submitted = false, answered = false, released = false, completed = false;
    QString originalChat, foregroundChat;
    QElapsedTimer elapsed; elapsed.start();
    const auto log = [&](const QString& text) { std::fprintf(stderr, "[%lld ms] %s\n", elapsed.elapsed(), text.toUtf8().constData()); std::fflush(stderr); };
    const auto fail = [&](const QString& error) { log("FAIL: " + error); result = 1; service.stopServers(); };
    QObject::connect(&service, &AssistantService::serversStopped, &app, &QCoreApplication::quit);
    QObject::connect(&service, &AssistantService::errorOccurred, fail);
    QObject::connect(&chats, &ChatManager::errorOccurred, fail);
    QObject::connect(&service, &AssistantService::serverStatusChanged, [&](const QString& name, const QString& state) { log(name+": "+state); });
    QObject::connect(service.imports(), &DocumentImportQueue::changed, [&]() {
        if (!enqueued || submitted) return;
        for (const auto& value : service.imports()->jobs()) {
            const auto job = value.toObject();
            if (job.value("status").toString() != "running" || !job.value("progress").toString().contains(QStringLiteral("Распознавание"))) continue;
            submitted = true;
            if (service.isBusy()) { fail("Background import blocks send controls"); return; }
            log("Import is recognizing images; create another chat and ask a question");
            foregroundChat = chats.createChat();
            return;
        }
    });
    QObject::connect(&service, &AssistantService::chatActivated, [&](const QString& id) {
        if (id == foregroundChat && submitted && !answered) {
            if (!chats.sendMessage(QStringLiteral("Ответь одним словом: готово."))) fail("Foreground question rejected");
        }
    });
    QObject::connect(service.imports(), &DocumentImportQueue::idle, [&]() {
        if (submitted && !answered && !service.imports()->jobs().isEmpty()) { released = true; log("Import yielded the model to chat"); }
    });
    QObject::connect(&service, &AssistantService::answerReady, [&](const QString& text) {
        if (!submitted || !released || completed) { fail("Unexpected foreground ordering"); return; }
        answered = true; log("ANSWER: " + text);
    });
    QObject::connect(service.imports(), &DocumentImportQueue::documentSaved, [&](const QString& id, const QString&) {
        if (!answered || id != originalChat) { fail("Import completed in wrong chat or before question"); return; }
        completed = true; log("Original import resumed and committed");
        QTimer::singleShot(0, &service, [&]() { chats.selectChat(originalChat); });
    });
    QObject::connect(&service, &AssistantService::chatActivated, [&](const QString& id) {
        if (!completed || id != originalChat) return;
        const auto* doc = service.documentManager()->documentAt(0);
        if (!doc || doc->text.isEmpty() || (doc->status != DocumentStatus::Ready && doc->status != DocumentStatus::ReadyWithWarnings)) {
            fail("Completed document not available after switching back"); return;
        }
        service.storage()->request("load_chat", {{"chatId",foregroundChat},{"remember",false}}, [&](const QJsonObject& value, const QString& error) {
            if (!error.isEmpty() || !value.value("documents").toArray().isEmpty()) { fail("Cross-chat document leak"); return; }
            result = 0; log("Real model preemption, answer, resume and isolation passed"); service.stopServers();
        });
    });
    QTimer kickoff;
    QObject::connect(&kickoff, &QTimer::timeout, [&]() {
        if (enqueued || !service.modelsReady() || service.isBusy() || chats.isBusy() || !chats.currentChat()) return;
        enqueued = true; originalChat = chats.currentChat()->id;
        service.loadDocument(app.arguments()[1]);
    });
    kickoff.start(100);
    QTimer::singleShot(360000, &service, [&]() { fail("Integration deadline exceeded"); });
    service.startServers(); app.exec();
    return result;
}
