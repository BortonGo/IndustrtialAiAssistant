// Manual integration check using real model servers and a disposable PostgreSQL cluster.
#include "application/assistant_service.hpp"
#include "chat/chat_manager.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <QJsonArray>
#include <QDebug>
#include <cstdio>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void waitFor(const std::function<bool()>& condition) {
    QElapsedTimer clock; clock.start();
    while (!condition() && clock.elapsed() < 300000) {
        QEventLoop loop; QTimer::singleShot(20, &loop, &QEventLoop::quit); loop.exec();
    }
    require(condition(), "Timed out waiting for attachments/model/storage");
}
void write(const QString& path, const QByteArray& content) {
    QFile file(path); require(file.open(QIODevice::WriteOnly), "Cannot create isolated fixture"); file.write(content);
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext&, const QString& message) {
        if (type != QtDebugMsg) std::fprintf(stderr, "%s\n", message.toUtf8().constData());
    });
    if (app.arguments().size() != 2) return 2;
    QDir().mkpath("build-storage-check");
    QTemporaryDir directory(QDir::current().filePath("build-storage-check/attachments-XXXXXX"));
    require(directory.isValid(), "No isolated directory");
    const QDir work(directory.path());
    qputenv("LOCAL_ASSISTANT_STORAGE_DIR", work.filePath("database").toUtf8());
    qputenv("LOCAL_ASSISTANT_STORAGE_PORT", "55436");
    AssistantService service; ChatManager chats(&service);
    QString error;
    QObject::connect(&chats, &ChatManager::errorOccurred, [&](const QString& text) { error = text; });
    QObject::connect(&service, &AssistantService::errorOccurred, [&](const QString& text) { error = text; });
    bool stopped = false;
    QObject::connect(&service, &AssistantService::serversStopped, [&]() { stopped = true; });
    try {
        const auto photo = work.filePath("attached-photo.png"), note = work.filePath("attached-note.txt");
        require(QFile::copy(app.arguments()[1], photo), "Cannot copy fixture image");
        write(note, QStringLiteral("Код проверки: ATTACHED-842. Это заметка к фотографии.").toUtf8());
        const auto library = work.filePath("unrelated-library.txt");
        write(library, QStringLiteral("Для совершенно другого устройства давление составляет 999 bar. Код OTHER-111.").toUtf8());
        service.startServers();
        waitFor([&]() { return service.modelsReady() && chats.currentChat() && !chats.isBusy() && service.isChatReady(chats.currentChat()->id); });
        const auto a = chats.currentChat()->id;
        service.loadDocument(library);
        waitFor([&]() { return !service.isBusy(); });
        require(error.isEmpty(), qPrintable(error));
        require(chats.sendMessage(QStringLiteral("Какое давление на этом фото и какой код в прикреплённой заметке? Ответь одним предложением."), {photo, note}), "Attachment question rejected");
        const auto b = chats.createChat();
        require(!b.isEmpty(), "Could not create chat during attachment import");
        waitFor([&]() { return !chats.isBusy() && !service.isBusy(); });
        require(error.isEmpty(), qPrintable(error));
        require(chats.currentChat()->id == b && chats.currentChat()->messages.empty() && service.documentManager()->documentCount() == 0,
                "Attachment operation leaked into another chat");
        chats.selectChat(a); waitFor([&]() { return service.isChatReady(a); });
        const auto& messages = chats.currentChat()->messages;
        require(messages.size() == 2 && messages[0].attachments.size() == 2, "Attachment metadata or answer missing");
        require(messages[1].text.contains("7.5") && messages[1].text.contains("ATTACHED-842") && !messages[1].text.contains("999"), "Model did not use both message attachments");
        require(!messages[0].contextSnapshot.contains("unrelated-library"), "Library file displaced explicit attachments");
        const auto image = messages[0].attachments[0];
        require(!image.documentId.isEmpty() && QFileInfo(image.previewPath).isFile(), "Persistent photo preview missing");
        qInfo().noquote() << "Photo + note:" << messages[1].text;

        require(chats.sendMessage(QStringLiteral("Какое здесь давление?"), {photo}), "Repeated attachment rejected");
        waitFor([&]() { return !chats.isBusy() && !service.isBusy(); });
        require(error.isEmpty(), qPrintable(error));
        require(chats.currentChat()->messages[2].attachments[0].documentId == image.documentId,
                "Repeated attachment duplicated its document");
        require(chats.currentChat()->messages.back().text.contains("7.5"), "Vague photo question lost attachment pixels");

        const auto broken = work.filePath("broken.pdf"); write(broken, "Not a PDF");
        const auto before = chats.currentChat()->messages.size();
        require(chats.sendMessage(QStringLiteral("Что в этом файле?"), {broken}), "Could not start broken attachment test");
        waitFor([&]() { return !chats.isBusy() && !service.isBusy(); });
        require(!error.isEmpty() && chats.currentChat()->messages.size() == before + 1 &&
                chats.currentChat()->messages.back().status == "failed", "Failed attachment still produced an answer");
        error.clear();

        const auto cancelled = work.filePath("cancelled.txt");
        write(cancelled, QStringLiteral("Тестовая запись для проверки отмены индексации вложенного документа.\n").repeated(500).toUtf8());
        bool cancellationRequested = false;
        auto cancel = QObject::connect(&service, &AssistantService::documentProgress, [&](const QString& status, bool canCancel) {
            if (canCancel && status.startsWith(QStringLiteral("Индексация:")) && !cancellationRequested) {
                cancellationRequested = true; QTimer::singleShot(0, &service, &AssistantService::cancelDocument);
            }
        });
        require(chats.sendMessage(QStringLiteral("Прочитай вложение"), {cancelled}), "Could not start attachment cancellation");
        waitFor([&]() { return !chats.isBusy() && !service.isBusy(); });
        QObject::disconnect(cancel);
        require(cancellationRequested && !error.isEmpty() && chats.currentChat()->messages.back().status == "failed", "Cancellation lost question state");

        // Refresh an already searchable file: cancellation and a failed DB write
        // must preserve both its previous text and its in-memory search entries.
        QString noteId, originalText;
        for (int i = 0; i < service.documentManager()->documentCount(); ++i) {
            const auto* document = service.documentManager()->documentAt(i);
            if (QFileInfo(document->sourcePath).fileName() == "attached-note.txt") {
                noteId = document->id; originalText = document->text;
            }
        }
        require(!noteId.isEmpty(), "No saved note for refresh checks");
        for (const bool failSave : {false, true}) {
            error.clear(); bool injected = false;
            write(note, QStringLiteral("Новая неподтверждённая версия: REPLACEMENT-999.\n").repeated(failSave ? 1 : 500).toUtf8());
            auto inject = QObject::connect(&service, &AssistantService::documentProgress,
                [&](const QString& status, bool canCancel) {
                if (injected) return;
                if (!failSave && canCancel && status.startsWith(QStringLiteral("Индексация:"))) {
                    injected = true; QTimer::singleShot(0, &service, &AssistantService::cancelDocument);
                } else if (failSave && status.startsWith(QStringLiteral("Сохранение документа и индекса"))) {
                    // Only this test's disposable source is removed, before save_document hashes it.
                    injected = QFile::remove(note);
                }
            });
            require(chats.sendMessage(QStringLiteral("Какой код в приложенной заметке?"), {note}), "Could not start refresh failure check");
            waitFor([&]() { return !chats.isBusy() && !service.isBusy(); });
            QObject::disconnect(inject);
            const auto* old = service.documentManager()->findDocument(noteId);
            require(injected && !error.isEmpty() && old && old->text == originalText && old->status == DocumentStatus::Ready,
                    "Failed refresh lost previous searchable document");
        }
        error.clear(); bool retrievedOld = false;
        auto observeOld = QObject::connect(&service, &AssistantService::retrievalReady,
            [&](const std::vector<VectorStore::SearchResult>& hits) {
            for (const auto& hit : hits) if (hit.chunk.documentId == noteId && hit.chunk.text.contains("ATTACHED-842")) retrievedOld = true;
        });
        require(chats.sendMessage(QStringLiteral("Какой код в attached-note.txt?")), "Query after failed refresh rejected");
        waitFor([&]() { return !chats.isBusy() && !service.isBusy(); });
        QObject::disconnect(observeOld);
        require(error.isEmpty() && retrievedOld, "Previous in-memory index was lost after refresh failure");
        bool checkedStored = false;
        service.storage()->request("load_chat", {{"chatId", a}, {"remember", false}}, [&](const QJsonObject& data, const QString& failure) {
            require(failure.isEmpty(), qPrintable(failure));
            for (const auto& value : data.value("documents").toArray()) if (value.toObject().value("id").toString() == noteId)
                checkedStored = value.toObject().value("text").toString() == originalText;
        });
        waitFor([&]() { return checkedStored; });

        write(note, QStringLiteral("Новый код проверки: UPDATED-905.").toUtf8());
        require(chats.sendMessage(QStringLiteral("Какой код в этой заметке?"), {note}), "Successful refresh rejected");
        waitFor([&]() { return !chats.isBusy() && !service.isBusy(); });
        require(error.isEmpty() && service.documentManager()->findDocument(noteId)->text.contains("UPDATED-905"),
                "Successful refresh did not commit the replacement document");
        bool retrievedNew = false;
        auto observeNew = QObject::connect(&service, &AssistantService::retrievalReady,
            [&](const std::vector<VectorStore::SearchResult>& hits) {
            for (const auto& hit : hits) if (hit.chunk.documentId == noteId && hit.chunk.text.contains("UPDATED-905")) retrievedNew = true;
        });
        require(chats.sendMessage(QStringLiteral("Какой код теперь в attached-note.txt?")), "Query after successful refresh rejected");
        waitFor([&]() { return !chats.isBusy() && !service.isBusy(); });
        QObject::disconnect(observeNew);
        require(error.isEmpty() && retrievedNew, "Successful refresh did not replace the searchable index");

        require(QFile::remove(photo) && QFileInfo(image.previewPath).isFile(), "Original removal lost saved photo");
        bool restored = false;
        service.storage()->request("bootstrap", {}, [&](const QJsonObject& data, const QString& failure) {
            require(failure.isEmpty(), qPrintable(failure));
            for (const auto& value : data.value("chats").toArray()) if (value.toObject().value("id").toString() == a) {
                const auto saved = value.toObject().value("messages").toArray()[0].toObject().value("attachments").toArray();
                require(saved.size() == 2 && QFileInfo(saved[0].toObject().value("previewPath").toString()).isFile(), "Stored attachments not restorable");
                restored = true;
            }
        });
        waitFor([&]() { return restored; });
        service.stopServers(); waitFor([&]() { return stopped; });
        qInfo() << "Message attachments: scoped photo+text, navigation, refresh rollback/commit, failure, cancellation and persistence passed";
        return 0;
    } catch (const std::exception& failure) {
        qCritical() << failure.what(); service.stopServers(); waitFor([&]() { return stopped; }); return 1;
    }
}
