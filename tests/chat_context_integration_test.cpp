// Manual test with real local models and a separate PostgreSQL data directory.
#include "application/assistant_service.hpp"
#include "chat/chat_manager.hpp"
#include "llm/context_builder.hpp"
#include <QCoreApplication>
#include <QTimer>
#include <QSettings>
#include <QDebug>
#include <QUuid>
#include <cstdio>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() < 2 || qgetenv("LOCAL_ASSISTANT_STORAGE_DIR").isEmpty()) return 2;
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext&, const QString& message) {
        if (type != QtDebugMsg) std::fprintf(stderr, "%s\n", message.toUtf8().constData());
    });
    AssistantService service;
    ChatManager chats(&service);
    QSettings settings("local-assistant.ini", QSettings::IniFormat);
    ContextBuilder builder({QUrl(QString("http://127.0.0.1:%1/v1/").arg(settings.value("models/chatPort", 8080).toInt())), "local-chat"});
    QString a, b, answerA, answerB;
    int stage = 0, exitCode = 1;
    bool seeded = false, failed = false;
    bool streamedInBackground = false;
    const bool resume = app.arguments().contains("--resume");
    auto check = [&](bool value, const QString& error) {
        if (!value && !failed) { failed = true; qCritical().noquote() << error; service.stopServers(); }
        return value;
    };
    auto seed = [&](const QString& chatId, const QString& code, std::function<void()> done) {
        const auto question = QUuid::createUuid().toString().mid(1, 36);
        service.storage()->request("save_question", {{"id",question},{"chatId",chatId},{"text","Код проекта " + code},{"title",code}},
            [&,question,code,done](const QJsonObject&, const QString& error) {
                if (!check(error.isEmpty(), error)) return;
                service.storage()->request("finish_question", {{"questionId",question},{"id",QUuid::createUuid().toString().mid(1,36)},
                                                               {"text","Принято. Код проекта " + code}},
                    [&,done](const QJsonObject&, const QString& error) { if (check(error.isEmpty(), error)) done(); });
            });
    };
    QObject::connect(&service, &AssistantService::serversStopped, &app, &QCoreApplication::quit);
    QObject::connect(&chats, &ChatManager::answerProgress, [&](const QString& id, const QString& text) {
        if (!check(chats.pendingAnswer(id) == text && chats.isAwaitingAnswer(id), "Preview lost original request ownership")) return;
        if (id != a || !chats.currentChat() || chats.currentChat()->id != b) return;
        if (!check(chats.pendingAnswer(b).isEmpty() && chats.currentChat()->messages.empty(), "Preview leaked into chat B")) return;
        if (!streamedInBackground) {
            streamedInBackground = true;
            chats.selectChat(a);
            if (!check(chats.pendingAnswer(a) == text, "Navigating back lost partial answer")) return;
            chats.selectChat(b);
        }
    });
    QObject::connect(&service, &AssistantService::errorOccurred, [&](const QString& error) { check(false, error); });
    QObject::connect(&chats, &ChatManager::errorOccurred, [&](const QString& error) { check(false, error); });
    QObject::connect(&service, &AssistantService::serverLogMessage, [&](const QString& name, const QString& message) {
        if (name == QStringLiteral("Контекст")) qInfo().noquote() << message;
    });
    QObject::connect(&chats, &ChatManager::messageAdded, [&](const QString& id, const QString&) {
        for (int i = 0; i < chats.chatCount(); ++i) {
            const auto* chat = chats.chatAt(i);
            if (chat->id != id || chat->messages.empty()) continue;
            const auto& message = chat->messages.back();
            if (message.role != MessageRole::Assistant || message.status != "complete") continue;
            if (id == a) answerA = message.text;
            if (id == b) answerB = message.text;
        }
    });
    QTimer step;
    QObject::connect(&step, &QTimer::timeout, [&]() {
        if (failed || !service.modelsReady() || !chats.currentChat()) return;
        const auto current = chats.currentChat()->id;
        const bool ready = service.isChatReady(current);
        switch (stage) {
        case 0:
            if (!ready || service.isBusy() || chats.isBusy()) return;
            if (resume) {
                a = current;
                if (!check(chats.currentChat()->messages.size() >= 4 && service.documentManager()->documentCount() == 1,
                           "Stored conversation and image did not survive restart")) return;
                if (!check(chats.sendMessage(QStringLiteral("Какой код проекта я назвал ранее? Ответь только кодом.")), "Restored question rejected")) return;
                b = chats.createChat(); stage = 20; break;
            }
            a = chats.createChat(); stage = 1; break;
        case 1:
            if (!ready || current != a) return;
            service.loadDocument(app.arguments()[1]);
            if (!check(service.isBusy(), "Document processing never started")) return;
            b = chats.createChat();
            if (!check(!b.isEmpty(), "Creating a chat during document processing was rejected")) return;
            stage = 2; break;
        case 2:
            if (!ready || current != b) return;
            if (!check(service.documentManager()->documentCount() == 0, "Background document appeared in chat B")) return;
            if (service.isBusy() || chats.isBusy()) return;
            qInfo() << "Switched away during document processing; chat B stayed empty";
            stage = 3;
            seed(a, "ALPHA-781", [&]() { seed(b, "BRAVO-926", [&]() { seeded = true; }); });
            break;
        case 3:
            if (!seeded) return;
            chats.selectChat(a); stage = 4; break;
        case 4:
            if (!ready || current != a) return;
            if (!check(service.documentManager()->documentCount() == 1, "Chat A lost its indexed image")) return;
            if (!check(chats.sendMessage(QStringLiteral("Какой код проекта я назвал ранее? Сначала назови код отдельным предложением. Затем объясни в трёх пунктах, зачем нужен код проекта.")), "Question A rejected")) return;
            // Navigation races with saving the question, then with query preparation.
            if (!check(chats.selectChat(b) && chats.selectChat(a) && chats.selectChat(b), "Navigation rejected while saving question")) return;
            stage = 5; break;
        case 5:
            if (!ready || answerA.isEmpty() || service.isBusy() || chats.isBusy()) return;
            if (!check(current == b && service.documentManager()->documentCount() == 0, "Late navigation reply replaced chat B")) return;
            qInfo().noquote() << "A:" << answerA;
            if (!check(answerA.contains("ALPHA-781") && !answerA.contains("BRAVO-926"), "History A was lost or mixed with B")) return;
            if (!check(streamedInBackground && chats.pendingAnswer(a).isEmpty(), "No early preview or completed preview was retained")) return;
            if (!check(chats.currentChat()->messages.empty(), "Answer A was inserted into B")) return;
            if (!check(chats.sendMessage(QStringLiteral("Какой код проекта я назвал ранее? Ответь только кодом.")), "Question B rejected")) return;
            chats.selectChat(a); stage = 6; break;
        case 6:
            if (!ready || answerB.isEmpty() || service.isBusy() || chats.isBusy()) return;
            if (!check(current == a && service.documentManager()->documentCount() == 1, "Chat A documents changed during answer B")) return;
            qInfo().noquote() << "B:" << answerB;
            if (!check(answerB.contains("BRAVO-926") && !answerB.contains("ALPHA-781"), "History B was lost or mixed with A")) return;
            stage = 7;
            {
                GenerationRequest request; request.contextSize = 4096; request.question = "Назови последние данные";
                for (int i = 0; i < 30; ++i) request.history.push_back({QStringLiteral("Вопрос %1 ").arg(i).repeated(100), QStringLiteral("Ответ %1 ").arg(i).repeated(100)});
                builder.build(request, [&,request](const PreparedContext& result, const QString& error) mutable {
                    if (!check(error.isEmpty(), error)) return;
                    if (!check(result.historyPairs > 0 && result.historyPairs < 30 &&
                               result.textTokens + result.imageReserve + result.maxAnswerTokens + ContextBuilder::safetyTokens <= result.contextSize,
                               "Real-tokenizer budget trimming failed")) return;
                    qInfo() << "Real tokenizer: kept pairs" << result.historyPairs << "text tokens" << result.textTokens;
                    request.history.clear(); request.question = QStringLiteral("Очень длинный вопрос ").repeated(6000);
                    builder.build(request, [&](const PreparedContext&, const QString& error) {
                        if (!check(!error.isEmpty(), "Oversized question was not rejected")) return;
                        exitCode = 0;
                        qInfo() << "Background navigation, original chat ownership, independent histories and real token budget passed";
                        service.stopServers();
                    });
                });
            }
            break;
        case 20:
            if (!ready || answerA.isEmpty() || service.isBusy() || chats.isBusy()) return;
            if (!check(current == b && chats.currentChat()->messages.empty() && service.documentManager()->documentCount() == 0,
                       "Restored answer or document leaked into the new chat")) return;
            if (!check(answerA.contains("ALPHA-781"), "Model did not receive restored history")) return;
            qInfo().noquote() << "After restart:" << answerA;
            chats.selectChat(a); stage = 21; break;
        case 21:
            if (!ready || current != a) return;
            exitCode = 0; qInfo() << "History after restart and background new-chat creation passed";
            service.stopServers(); stage = 22; break;
        default: break;
        }
    });
    step.start(20);
    QTimer::singleShot(600000, &service, [&]() { check(false, "Chat/context integration test timed out"); });
    QTimer::singleShot(0, &service, &AssistantService::startServers);
    app.exec();
    return exitCode;
}
