// Manual integration check: uses the configured local models, never a remote endpoint.
#include "application/assistant_service.hpp"
#include "chat/chat_manager.hpp"
#include <QCoreApplication>
#include <QTimer>
#include <QDebug>
#include <QDir>
#include <cstdio>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("LocalAssistantServiceTests");
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext&, const QString& message) {
        if (type == QtDebugMsg) return;
        std::fprintf(stderr, "%s\n", message.toUtf8().constData());
    });
    if (app.arguments().size() < 2) return 2;
    AssistantService service;
    ChatManager chats(&service);
    int result = 1;
    const bool restoreOnly = app.arguments().contains("--restore");
    int restoreStep = 0;
    QString restoredChatId;
    bool started = false, queried = false, foundImage = false;
    QObject::connect(&service, &AssistantService::serversStopped, &app, &QCoreApplication::quit);
    QObject::connect(&service, &AssistantService::serverStatusChanged,
                     [](const QString& name, const QString& status) { qInfo().noquote() << name << status; });
    QObject::connect(&service, &AssistantService::documentProgress,
                     [](const QString& text, bool) { qInfo().noquote() << text; });
    QObject::connect(&service, &AssistantService::errorOccurred,
                     [&](const QString& text) { qWarning().noquote() << text; QTimer::singleShot(0, &service, &AssistantService::stopServers); });
    auto start = [&]() {
        if (restoreOnly) return;
        if (!service.modelsReady() || chats.isBusy() || started) return;
        started = true;
        chats.createChat();
    };
    QObject::connect(&service, &AssistantService::modelsReadyChanged, [&](bool) { QTimer::singleShot(0, &service, start); });
    QObject::connect(&chats, &ChatManager::operationChanged, [&]() { QTimer::singleShot(0, &service, start); });
    QObject::connect(&chats, &ChatManager::currentChatChanged, [&](const QString& chatId) {
        if (restoreOnly) {
            const auto* chat = chats.currentChat();
            const auto* document = service.documentManager()->documentAt(0);
            if (restoreStep == 1) {
                if (service.documentManager()->documentCount() != 0 || !chat->messages.empty()) { service.stopServers(); return; }
                ++restoreStep;
                QTimer::singleShot(0, &service, [&]() { chats.selectChat(restoredChatId); });
                return;
            }
            const bool valid = chat && chat->messages.size() >= 2 && document &&
                    (document->status == DocumentStatus::Ready || document->status == DocumentStatus::ReadyWithWarnings) &&
                    chat->messages.back().text.contains("7.5") && !chat->messages.front().contextSnapshot.isEmpty();
            if (!valid) { qCritical() << "Restore failed"; service.stopServers(); return; }
            if (restoreStep == 0) {
                restoredChatId = chatId; ++restoreStep;
                QTimer::singleShot(0, &service, [&]() { chats.createChat(); });
            } else {
                result = 0; qInfo() << "Restart, stored sources, chat isolation and switching back passed"; service.stopServers();
            }
            return;
        }
        if (started) service.loadDocument(app.arguments()[1]);
    });
    QObject::connect(&service, &AssistantService::busyChanged, [&](bool busy) {
        if (busy || !started || queried) return;
        const auto* document = service.documentManager()->documentAt(0);
        if (!document || document->status != DocumentStatus::Ready) return;
        queried = true;
        QTimer::singleShot(0, &service, [&]() {
            if (!chats.sendMessage(QStringLiteral("Какое давление указано на табличке PUMP-417?"))) {
                qCritical() << "Query rejected"; service.stopServers();
            }
        });
    });
    QObject::connect(&service, &AssistantService::retrievalReady,
                     [&](const std::vector<VectorStore::SearchResult>& hits) {
        for (const auto& hit : hits) {
            if (!hit.chunk.imagePath.isEmpty()) foundImage = true;
            qInfo().noquote() << hit.chunk.sourceDescription();
        }
    });
    QObject::connect(&service, &AssistantService::answerReady, [&](const QString& answer) {
        qInfo().noquote() << "ANSWER:" << answer;
        result = foundImage && QString(answer).replace(',', '.').contains("7.5") ? 0 : 1;
    });
    QObject::connect(&chats, &ChatManager::messageAdded, [&](const QString&, const QString&) {
        const auto* chat = chats.currentChat();
        if (chat && !chat->messages.empty() && chat->messages.back().role == MessageRole::Assistant) {
            if (chat->messages.back().status != "complete") result = 1;
            QTimer::singleShot(0, &service, &AssistantService::stopServers);
        }
    });
    QObject::connect(&service, &AssistantService::questionFailed, [&](const QString& error) {
        qCritical().noquote() << error;
        service.stopServers();
    });
    QTimer::singleShot(600000, &service, [&]() { qCritical() << "Integration test timed out"; service.stopServers(); });
    QTimer::singleShot(0, &service, &AssistantService::startServers);
    app.exec();
    std::fprintf(stderr, "Service pipeline result: %d\n", result);
    return result;
}
