#include "llm/context_builder.hpp"
#include "llm/context_selection.hpp"
#include "llm/lmstudio_llm_client.hpp"
#include "llm/system_status_policy.hpp"
#include "llm/completion_stream.hpp"
#include <QCoreApplication>
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QEventLoop>
#include <QTimer>
#include <QTemporaryDir>
#include <QFile>
#include <QDebug>
#include <cstdio>
#include <memory>
#include <stdexcept>

static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

// Deterministic HTTP fixture: tests selection and failure handling, not a real tokenizer.
class TokenServer : public QTcpServer {
public:
    bool broken = false;
    int calls = 0;
    int completions = 0;
    bool holdCompletions = false;
    QString generatedAnswer = "Test answer";
    QString answerFinishReason = "stop";
    bool truncateStream = false;
    bool badStream = false;
    bool failHttp = false;
    int streamInterval = 1;
    QString selection = "{\"context\":\"conversation\",\"sources\":[],\"history\":[0],\"system_status\":false}";
    QString conversationPlan = "{\"memory\":[0],\"anchor\":0,\"query\":\"PUMP-417 pressure\"}";
    QJsonObject lastAnswerRequest;
    TokenServer() {
        connect(this, &QTcpServer::newConnection, this, [this]() {
            while (hasPendingConnections()) {
                auto* socket = nextPendingConnection();
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer]() {
                    *buffer += socket->readAll();
                    const int headerEnd = buffer->indexOf("\r\n\r\n");
                    if (headerEnd < 0) return;
                    int size = 0;
                    for (const auto& line : buffer->left(headerEnd).split('\n'))
                        if (line.toLower().startsWith("content-length:")) size = line.mid(15).trimmed().toInt();
                    if (buffer->size() < headerEnd + 4 + size) return;
                    ++calls;
                    const auto body = QJsonDocument::fromJson(buffer->mid(headerEnd + 4, size)).object();
                    QJsonObject response;
                    if (!broken && buffer->startsWith("POST /apply-template ")) {
                        QString prompt;
                        for (const auto& item : body["messages"].toArray()) {
                            const auto message = item.toObject();
                            prompt += "<role>" + message["role"].toString() + "\n";
                            const auto content = message["content"];
                            if (content.isString()) prompt += content.toString();
                            else for (const auto& part : content.toArray())
                                prompt += part.toObject()["type"] == "text" ? part.toObject()["text"].toString() : "<image>";
                            prompt += "</role>\n";
                        }
                        response["prompt"] = prompt + "<assistant>";
                    } else if (!broken && buffer->startsWith("POST /tokenize ")) {
                        QJsonArray tokens;
                        for (int i = 0; i < (body["content"].toString().size() + 3) / 4; ++i) tokens.append(i);
                        response["tokens"] = tokens;
                    } else if (!broken && buffer->startsWith("POST /v1/chat/completions ")) {
                        ++completions;
                        if (holdCompletions) return;
                        const bool selecting = body.contains("response_format");
                        if (!selecting) {
                            lastAnswerRequest = body;
                            require(body.value("stream").toBool(), "Answer was not requested as a stream");
                            auto bytes = std::make_shared<QByteArray>();
                            const auto frame = [&](const QJsonObject& delta, const QJsonValue& reason = QJsonValue()) {
                                *bytes += "data: " + QJsonDocument(QJsonObject{{"choices", QJsonArray{QJsonObject{
                                    {"index", 0}, {"delta", delta}, {"finish_reason", reason}}}}}).toJson(QJsonDocument::Compact) + "\r\n\r\n";
                            };
                            *bytes += ": keep-alive\r\n\r\n";
                            frame({{"role", "assistant"}});
                            for (int i = 0; i < generatedAnswer.size(); i += 4) frame({{"content", generatedAnswer.mid(i, 4)}});
                            if (!truncateStream) {
                                frame({}, answerFinishReason);
                                *bytes += "data: {\"choices\":[],\"usage\":{}}\n\ndata: [DONE]\n\n";
                            }
                            if (badStream) *bytes = "data: {broken}\n\n";
                            if (failHttp) *bytes = "{\"error\":{\"message\":\"test unavailable\"}}";
                            socket->write(failHttp ? "HTTP/1.1 503 Service Unavailable\r\n" : "HTTP/1.1 200 OK\r\n");
                            socket->write("Content-Type: text/event-stream\r\nConnection: close\r\nContent-Length: " +
                                          QByteArray::number(bytes->size()) + "\r\n\r\n");
                            auto offset = std::make_shared<int>(0);
                            auto* sender = new QTimer(socket);
                            connect(sender, &QTimer::timeout, socket, [socket, bytes, offset, sender]() {
                                socket->write(bytes->mid(*offset, 17)); *offset += 17;
                                if (*offset >= bytes->size()) { sender->stop(); socket->disconnectFromHost(); }
                            });
                            sender->start(streamInterval);
                            return;
                        }
                        response["choices"] = QJsonArray{QJsonObject{{"finish_reason", selecting ? QString("stop") : answerFinishReason},
                            {"message", QJsonObject{{"role", "assistant"}, {"content", selecting ?
                                (body["response_format"].toObject()["schema"].toObject()["properties"].toObject().contains("anchor") ? conversationPlan : selection)
                                : generatedAnswer}}}}};
                    }
                    const auto bytes = QJsonDocument(response).toJson(QJsonDocument::Compact);
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
                                  QByteArray::number(bytes.size()) + "\r\n\r\n" + bytes);
                    socket->disconnectFromHost();
                });
            }
        });
        require(listen(QHostAddress::LocalHost, 0), "Cannot listen for context tests");
    }
};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString& text) { std::fprintf(stderr, "%s\n", text.toUtf8().constData()); });
    try {
        const QByteArray event = QStringLiteral("data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Привет 🌍\"},\"finish_reason\":null}]}\r\n\r\n"
            "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n").toUtf8();
        CompletionStream decoder;
        for (const auto byte : event) decoder.feed(QByteArray(1, byte));
        require(decoder.complete() && decoder.text() == QStringLiteral("Привет 🌍"), "Fragmented UTF-8/SSE corrupted");
        CompletionStream incomplete; incomplete.feed(event.left(event.size() - 3));
        require(!incomplete.complete(), "Truncated SSE accepted");
        decoder.feed("data: {}\n\n"); require(!decoder.error().isEmpty(), "Data after DONE accepted");
        const auto noisyStream = QStringLiteral("Тебя зовут Лиза. Сейчас CPU 5.8%, RAM 64.1%.\nГотово.\n");
        for (int i = 0; i <= noisyStream.size(); ++i) {
            const auto preview = streamPreview(noisyStream.left(i), false, {});
            require(!preview.contains("CPU") && !preview.contains("RAM") && !preview.contains("5.8"), "Telemetry leaked during streaming");
        }
        TokenServer server;
        for (const auto& question : {QStringLiteral("Как меня зовут?"), QStringLiteral("Что такое загрузка CPU?"),
                 QStringLiteral("Не показывай состояние системы. Как меня зовут?"),
                 QStringLiteral("Какая допустимая загрузка CPU по инструкции?"), QStringLiteral("Привет")})
            require(!requestsSystemStatus(question), "Unrequested MCP access allowed");
        for (const auto& question : {QStringLiteral("Какая сейчас загрузка компьютера?"), QStringLiteral("Загрузка CPU?"),
                 QStringLiteral("Сколько занято оперативной памяти?"), QStringLiteral("Покажи состояние системы")})
            require(requestsSystemStatus(question), "Explicit MCP question blocked");
        const std::vector<ConversationTurn> monitoring{{QStringLiteral("Какая загрузка CPU?"), "CPU 5.8%"}};
        require(requestsSystemStatus(QStringLiteral("А память?"), monitoring), "Monitoring follow-up blocked");
        require(!requestsSystemStatus(QStringLiteral("Как меня зовут?"), monitoring), "Old monitoring question opted in a new topic");
        const QString noisy = QStringLiteral("Тебя зовут Лиза. Сейчас система работает стабильно: CPU — 5.8%, RAM — 64.1% "
            "(последнее обновление — 0 секунд назад).\n\nТвой проект — Северный маяк.");
        const auto clean = withoutSystemStatus(noisy);
        require(clean.contains(QStringLiteral("Лиза")) && clean.contains(QStringLiteral("Северный маяк")) &&
                !clean.contains("CPU") && !clean.contains("64.1") && !clean.contains(QStringLiteral("стабильно")), "Telemetry sanitation lost facts or retained metrics");
        const QString code = "```python\nprint('CPU 50%')\n```";
        require(withoutSystemStatus(code) == code, "Telemetry filter damaged a code example");
        const QString definition = QStringLiteral("Оперативная память (RAM) хранит данные программ. Давление — 7.5 bar.");
        require(withoutSystemStatus(definition) == definition, "Telemetry filter damaged general information");
        const QString documentFact = QStringLiteral("По инструкции допустимая загрузка CPU — до 75%.");
        require(withoutSystemStatus(documentFact, QStringLiteral("Инструкция: CPU 75 %")) == documentFact,
                "Telemetry filter removed documented measurements");
        require(withoutSystemStatus(QStringLiteral("Сейчас CPU 75%."), QStringLiteral("Инструкция: CPU 75 %")).isEmpty(),
                "Document measurements allowed unsolicited live monitoring");
        require(withoutSystemStatus(QStringLiteral("**Состояние системы:**\n- Загрузка процессора: 5.8 %\n- Использование RAM: 64.1 %")).isEmpty(),
                "Markdown monitoring block remained");
        ContextBuilder builder({QUrl(QString("http://127.0.0.1:%1/v1/").arg(server.serverPort())), "test"});
        auto run = [&](const GenerationRequest& request, bool success = true,
                       ContextBuilder::Purpose purpose = ContextBuilder::Purpose::Answer) {
            PreparedContext result; QString error; bool done = false;
            QEventLoop loop; QTimer timer; timer.setSingleShot(true);
            QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
            builder.build(request, [&](const PreparedContext& value, const QString& failure) {
                result = value; error = failure; done = true; loop.quit();
            }, purpose);
            timer.start(10000); if (!done) loop.exec();
            require(done, "Context builder timed out");
            if (success != error.isEmpty()) qWarning().noquote() << "Context result:" << error << "expected success:" << success;
            require(success == error.isEmpty(), "Unexpected context result");
            if (success) require(result.textTokens + result.imageReserve + result.maxAnswerTokens + ContextBuilder::safetyTokens <= result.contextSize,
                                 "Context exceeds budget");
            return result;
        };
        GenerationRequest request; request.contextSize = 4096; request.question = "Current question";
        request.systemStatus = "Current MCP only";
        request.sources.push_back({"SOURCE A", {}});
        for (int i = 0; i < 50; ++i) request.history.push_back({QString("Q%1:").arg(i) + QString(500, 'q'), QString(700, 'a')});
        const auto result = run(request);
        require(result.historyPairs > 0 && result.historyPairs < 50, "History was not trimmed");
        require(result.messages.size() == 2 + result.historyPairs * 2, "Incomplete history pair");
        require(result.messages[1].toObject()["content"].toString().startsWith(QString("Q%1:").arg(50 - result.historyPairs)), "Oldest history retained instead of newest");
        require(result.messages[result.messages.size() - 2].toObject()["role"] == "assistant", "Assistant pair missing");
        require(server.calls < 22, "History selection performs too many requests");

        GenerationRequest identity;
        identity.question = QStringLiteral("Как меня зовут?");
        identity.history = {{QStringLiteral("Меня зовут Марина."), QStringLiteral("Привет, Марина!")},
                            {QStringLiteral("А тебя как зовут?"), QStringLiteral("Я Local Assistant.")}};
        const auto conversation = run(identity);
        require(conversation.historyPairs == 2, "Personal conversation history lost");
        require(conversation.messages[0].toObject()["role"] == "system", "System role lost");
        for (int i = 0; i < 2; ++i) {
            const auto user = conversation.messages[1 + i * 2].toObject();
            const auto assistant = conversation.messages[2 + i * 2].toObject();
            require(user["role"] == "user" && user["content"] == identity.history[i].question,
                    "User's statement attributed to another speaker");
            require(assistant["role"] == "assistant" && assistant["content"] == identity.history[i].answer,
                    "Assistant's statement attributed to another speaker");
        }
        require(conversation.messages.last().toObject()["role"] == "user" &&
                conversation.messages.last().toObject()["content"].toString().endsWith("\n" + identity.question),
                "Current question changed role or text");

        QTemporaryDir directory;
        QFile picture(directory.path() + "/picture.png"); require(picture.open(QIODevice::WriteOnly), "Image fixture failed");
        picture.write("fixture bytes"); picture.close();
        request.sources = {{"IMAGE SOURCE", picture.fileName()}, {"Same image", picture.fileName()}};
        const auto images = run(request);
        require(images.imageReserve == ContextBuilder::imageTokenReserve, "Same image counted twice");
        require(images.historyPairs < result.historyPairs, "Image reserve did not reduce history");
        const auto multimodal = images.messages.last().toObject()["content"].toArray();
        require(multimodal.last().toObject()["type"] == "text" &&
                multimodal.last().toObject()["text"].toString().endsWith(request.question),
                "Current question must follow reference images");

        request.sources = {{"Keep this source", {}}, {QString(40000, 'x'), picture.fileName()}};
        request.sources[0].location = QJsonObject{{"documentId", "kept"}, {"pageFirst", 12}};
        request.sources[1].location = QJsonObject{{"documentId", "dropped"}};
        const auto trimmed = run(request);
        require(trimmed.sourceCount == 1 && trimmed.imageReserve == 0, "Lowest ranked oversized source was not removed");
        require(trimmed.sourcesSnapshot.contains("Keep this source"), "Useful source lost");
        require(trimmed.sourceReferences.size() == 1 && trimmed.sourceReferences[0].toObject()["documentId"] == "kept" &&
                trimmed.sourceReferences[0].toObject()["text"] == "Keep this source" &&
                trimmed.sourceReferences[0].toObject()["pageFirst"] == 12 &&
                trimmed.sourceReferences[0].toObject()["number"] == 1 &&
                trimmed.sourcesSnapshot.contains(QStringLiteral("[Источник 1]")), "References do not match final budgeted evidence");
        require(conversation.sourceReferences.isEmpty(), "Conversation acquired document sources");
        request.sources = {{QString(40000, 'x'), {}, "Table summary", "table-a", true}};
        run(request, false); // Do not silently drop the explicitly requested complete table.
        const auto summary = run(request, true, ContextBuilder::Purpose::Selection);
        require(summary.sourceCount == 1 && QJsonDocument(summary.messages).toJson().contains("Table summary") &&
                !QJsonDocument(summary.messages).toJson().contains(QByteArray(20000, 'x')),
                "Selection used entire expanded table instead of its descriptor");
        request.sources = {{"Small source", {}}, {QString(40000, 'x'), {}, "Group summary", "section"},
                           {"Related image", picture.fileName(), "Image descriptor", "section"}};
        const auto groupTrimmed = run(request);
        require(groupTrimmed.sourceCount == 1 && groupTrimmed.imageReserve == 0, "Budget split a section/image group");
        request.question = QString(40000, 'q'); run(request, false);
        request.question = "Small"; request.history.clear(); request.sources.clear();
        server.broken = true; run(request, false); server.broken = false;

        bool staleCallback = false;
        builder.build(request, [&](const PreparedContext&, const QString&) { staleCallback = true; });
        builder.cancel();
        run(request);
        require(!staleCallback, "Cancelled build called its callback");

        identity.sources = {{"Unrelated plate PUMP-417", "missing-image.png"}, {"Unrelated document", {}}};
        identity.systemStatus = "Fresh CPU/RAM: 3.8/63.5";
        identity.history[0].answer += " CPU 5.8%, RAM 64.1%.";
        const auto offered = run(identity, true, ContextBuilder::Purpose::Selection);
        require(offered.imageReserve == 0, "Selector should not read or encode images");
        require(!QJsonDocument(offered.messages).toJson().contains("64.1"), "Old telemetry entered the selector");
        const auto selected = selectContext(identity, offered, server.selection);
        require(selected.sources.empty() && selected.systemStatus.isEmpty() && selected.history.size() == 1,
                "Unrelated sources, metrics or history leaked through selection");
        require(selected.history[0].question == identity.history[0].question && selected.question == identity.question,
                "Selection rewrote user facts");
        const auto gated = selectContext(identity, offered,
            "{\"context\":\"conversation\",\"sources\":[0],\"history\":[],\"system_status\":true}");
        require(gated.sources.empty() && gated.systemStatus.isEmpty(), "Conversation category did not exclude external evidence");
        auto attachmentRequest = identity;
        attachmentRequest.question = QStringLiteral("Как это исправить?");
        attachmentRequest.attachedFiles = QStringList{"photo.png"};
        attachmentRequest.sources = {{"Attached picture", picture.fileName(), "Photo", "attachment", true}};
        const auto attachmentOffered = run(attachmentRequest, true, ContextBuilder::Purpose::Selection);
        const auto attachmentSelected = selectContext(attachmentRequest, attachmentOffered,
            "{\"context\":\"conversation\",\"sources\":[],\"history\":[],\"system_status\":false}");
        require(attachmentSelected.sources.size() == 1, "Selector dropped an explicit message attachment");
        const auto attachedAnswer = run(attachmentSelected);
        require(attachedAnswer.imageReserve == ContextBuilder::imageTokenReserve, "Attached pixels did not reach generation");
        require(attachedAnswer.messages.last().toObject()["content"].toArray()[0].toObject()["text"].toString().contains("photo.png"),
                "Attached image has no explicit current-message scope");
        auto rejects = [&](const QString& json, PreparedContext available) {
            bool rejected = false;
            try { selectContext(identity, available, json); } catch (const std::exception&) { rejected = true; }
            require(rejected, "Invalid selection accepted");
        };
        rejects("not json", offered);
        rejects("{\"context\":\"conversation\",\"sources\":[0.5],\"history\":[],\"system_status\":false}", offered);
        rejects("{\"context\":\"conversation\",\"sources\":[2],\"history\":[],\"system_status\":false}", offered);
        rejects("{\"context\":\"conversation\",\"sources\":[0,0],\"history\":[],\"system_status\":false}", offered);
        rejects("{\"context\":\"conversation\",\"sources\":[],\"history\":[0],\"system_status\":\"false\"}", offered);
        auto truncated = offered; truncated.historyPairs = 1;
        rejects(server.selection, truncated); // ID 0 was outside the measured selection window.
        const auto mixed = selectContext(identity, offered,
            "{\"context\":\"mixed\",\"sources\":[1],\"history\":[1,0],\"system_status\":true}");
        require(mixed.sources.size() == 1 && mixed.systemStatus == identity.systemStatus &&
                mixed.history[0].question == identity.history[0].question, "Mixed context or chronology lost");
        auto monitoringRequest = identity;
        monitoringRequest.statusAccess = SystemStatusAccess::Requested;
        const auto followUp = selectContext(monitoringRequest, offered,
            "{\"context\":\"conversation\",\"sources\":[],\"history\":[0],\"system_status\":false}");
        require(followUp.systemStatus == identity.systemStatus, "Explicit monitoring lost fresh readings to selector");

        LMStudioLLMClient client({QUrl(QString("http://127.0.0.1:%1/v1/").arg(server.serverPort())), "test"});
        QStringList previews;
        QObject::connect(&client, &ILLMClient::answerProgress, [&](const QString& text) {
            require(!text.contains("CPU") && !text.contains("64.1"), "Streaming preview leaked unsolicited telemetry");
            previews.append(text);
        });
        auto runClient = [&](bool success, bool expectPrepared = true, bool truncatedAnswer = false) {
            QEventLoop loop; QTimer timer; timer.setSingleShot(true);
            bool done = false, ok = false, prepared = false;
            auto ready = QObject::connect(&client, &ILLMClient::contextPrepared, [&](const QString& snapshot, const QString&) {
                require(snapshot.isEmpty(), "Unselected evidence persisted in source snapshot");
                prepared = true; client.generatePrepared();
            });
            auto answered = QObject::connect(&client, &ILLMClient::answerReady, [&](const QString& answer) {
                require(!answer.contains("CPU") && !answer.contains("64.1"), "Generated unsolicited telemetry reached UI/storage");
                require(answer.contains(QStringLiteral("Ответ не завершён")) == truncatedAnswer,
                        "Output token limit was hidden or invented");
                done = ok = true; loop.quit();
            });
            auto failed = QObject::connect(&client, &ILLMClient::errorOccurred, [&](const QString&) { done = true; loop.quit(); });
            QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
            client.prepare(identity); timer.start(10000); loop.exec();
            QObject::disconnect(ready); QObject::disconnect(answered); QObject::disconnect(failed);
            require(done && ok == success && prepared == expectPrepared, "Unexpected selection/generation lifecycle");
        };
        runClient(true);
        const auto sent = server.lastAnswerRequest["messages"].toArray();
        require(sent.size() == 4 && sent[1].toObject()["content"] == identity.history[0].question,
                "Selected history not sent to generation");
        require(!QJsonDocument(sent).toJson().contains("PUMP-417") && !QJsonDocument(sent).toJson().contains("3.8/63.5"),
                "Unselected reference data reached answer generation");
        server.selection = "{\"context\":\"conversation\",\"sources\":[999],\"history\":[],\"system_status\":false}";
        const int before = server.completions;
        runClient(false, false);
        require(server.completions == before + 1, "Invalid selection fell back to sending all evidence");

        server.holdCompletions = true;
        server.selection = "{\"context\":\"conversation\",\"sources\":[],\"history\":[0],\"system_status\":false}";
        bool cancelledSignal = false, selectionStarted = false;
        auto unexpectedReady = QObject::connect(&client, &ILLMClient::contextPrepared,
            [&](const QString&, const QString&) { cancelledSignal = true; });
        auto unexpectedError = QObject::connect(&client, &ILLMClient::errorOccurred,
            [&](const QString&) { cancelledSignal = true; });
        const int callsBeforeCancel = server.completions;
        QEventLoop cancelLoop; QTimer poll, timeout;
        QObject::connect(&poll, &QTimer::timeout, [&]() {
            if (server.completions <= callsBeforeCancel) return;
            poll.stop(); selectionStarted = true; client.cancelRequests();
            QTimer::singleShot(30, &cancelLoop, &QEventLoop::quit);
        });
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &cancelLoop, &QEventLoop::quit);
        client.prepare(identity); poll.start(5); timeout.start(10000); cancelLoop.exec();
        QObject::disconnect(unexpectedReady); QObject::disconnect(unexpectedError);
        require(selectionStarted && !cancelledSignal, "Cancelled selector delivered a stale result");
        server.holdCompletions = false;
        server.generatedAnswer = QStringLiteral("Тебя зовут Лиза. Сейчас система работает стабильно: CPU 5.8%, RAM 64.1%.");
        runClient(true);
        server.generatedAnswer = "CPU 5.8%, RAM 64.1%.";
        runClient(false);
        server.generatedAnswer = QStringLiteral("Начало подробного ответа.");
        server.answerFinishReason = "length";
        runClient(true, true, true);
        require(!previews.empty(), "No partial output delivered");
        server.answerFinishReason = "stop";
        server.truncateStream = true; runClient(false); server.truncateStream = false;
        server.badStream = true; runClient(false); server.badStream = false;
        server.failHttp = true; runClient(false); server.failHttp = false;
        server.generatedAnswer = QStringLiteral("Первая строка.\nВторая строка.\nТретья строка.\n");
        server.streamInterval = 5;
        bool cancelled = false, stale = false;
        QEventLoop streamLoop;
        auto streamReady = QObject::connect(&client, &ILLMClient::contextPrepared, [&](const QString&, const QString&) { client.generatePrepared(); });
        auto streamProgress = QObject::connect(&client, &ILLMClient::answerProgress, [&](const QString&) {
            if (cancelled) { stale = true; return; }
            cancelled = true; client.cancelRequests(); QTimer::singleShot(300, &streamLoop, &QEventLoop::quit);
        });
        auto streamFinished = QObject::connect(&client, &ILLMClient::answerReady, [&](const QString&) { stale = true; streamLoop.quit(); });
        auto streamError = QObject::connect(&client, &ILLMClient::errorOccurred, [&](const QString&) { stale = true; streamLoop.quit(); });
        QTimer::singleShot(10000, &streamLoop, &QEventLoop::quit);
        client.prepare(identity); streamLoop.exec();
        QObject::disconnect(streamReady); QObject::disconnect(streamProgress); QObject::disconnect(streamFinished); QObject::disconnect(streamError);
        require(cancelled && !stale, "Cancelled streaming request delivered stale events or no early progress");
        const QJsonArray memory{QJsonObject{{"questionId","user-old"},{"text",QStringLiteral("Меня зовут Алексей.")}}};
        const QJsonObject memoryState{{"compactThrough",1},{"candidates",memory}, {"pairs",QJsonArray{
            QJsonObject{{"question",QStringLiteral("Где находится PUMP-417?")},{"answer","Room 218. CPU 88%."}}}}};
        require(needsFollowupQuery(QStringLiteral("А какое у него давление?")), "Follow-up was not detected");
        require(!needsFollowupQuery(QStringLiteral("Объясни сортировку массива")), "Standalone question became follow-up");
        const auto planInput = QJsonDocument(conversationPlanMessages("Pressure?",memoryState,true)).toJson();
        require(!planInput.contains("CPU 88"), "Telemetry leaked into conversation planning");
        auto runPlan = [&](int size) {
            QJsonObject result; QString failure; bool done = false;
            QEventLoop loop;
            client.planConversation(QStringLiteral("А давление?"), memoryState, size, true,
                [&](const QJsonObject& value, const QString& error) { result=value; failure=error; done=true; loop.quit(); });
            QTimer::singleShot(5000,&loop,&QEventLoop::quit);
            if (!done) loop.exec();
            require(done,"Conversation planning callback lost");
            return std::make_pair(result,failure);
        };
        auto planned = runPlan(8192);
        require(planned.second.isEmpty() && planned.first["anchor"].toInt() == 0, "Conversation plan failed");
        const int plannedCalls = server.completions;
        require(!runPlan(200).second.isEmpty() && server.completions == plannedCalls, "Oversized memory sent to model");
        server.conversationPlan = "{\"memory\":[999],\"anchor\":0,\"query\":\"bad\"}";
        require(!runPlan(8192).second.isEmpty(), "Fabricated memory ID accepted");
        server.conversationPlan = "{\"memory\":[0],\"anchor\":99,\"query\":\"bad\"}";
        require(!runPlan(8192).second.isEmpty(), "Foreign follow-up turn accepted");
        const auto memoryOnly = validateConversationPlan("{\"memory\":[0],\"anchor\":999,\"query\":\"ignored\"}",
            memoryState,false,"Name?");
        require(memoryOnly["memory"].toArray().size()==1 && memoryOnly["anchor"].toInt()==-1 && memoryOnly["query"].toString().isEmpty(),
                "Disabled follow-up discarded valid memory or changed the query");
        auto followupState=memoryState; followupState["compactThrough"]=0; followupState["candidates"]=QJsonArray{};
        const auto followupOnly=validateConversationPlan("{\"memory\":[999],\"anchor\":0,\"query\":\"PUMP-417 pressure?\"}",
            followupState,true,"Its pressure?");
        require(followupOnly["memory"].toArray().isEmpty() && followupOnly["anchor"].toInt()==0,
                "Inactive memory update invalidated the follow-up");
        const auto newTopic = validateConversationPlan("{\"memory\":[0],\"anchor\":0,\"query\":\"How to cook buckwheat?\"}",
            memoryState,true,"And how to cook buckwheat?");
        require(newTopic["anchor"].toInt() == -1, "Unrelated question inherited document scope");
        const auto inventedCode = validateConversationPlan("{\"memory\":[0],\"anchor\":0,\"query\":\"PUMP-999 pressure\"}",
            memoryState,true,"And its pressure?");
        require(inventedCode["anchor"].toInt() == -1, "Invented device code accepted");
        GenerationRequest remembered;
        remembered.question = QStringLiteral("Как меня зовут?"); remembered.memory = memory;
        remembered.contextSize=8192; remembered.maxAnswerTokens=512;
        QEventLoop memoryLoop; PreparedContext offeredMemory; QString memoryError;
        builder.build(remembered,[&](const PreparedContext& value, const QString& error) { offeredMemory=value; memoryError=error; memoryLoop.quit(); });
        QTimer::singleShot(5000,&memoryLoop,&QEventLoop::quit); memoryLoop.exec();
        require(memoryError.isEmpty() && offeredMemory.memoryItems == 1, "Memory absent from token budget");
        require(offeredMemory.textTokens + offeredMemory.maxAnswerTokens + ContextBuilder::safetyTokens <= offeredMemory.contextSize, "Memory exceeded context budget");
        const auto selectedMemory = selectContext(remembered,offeredMemory,
            "{\"context\":\"conversation\",\"sources\":[],\"history\":[],\"memory\":[0],\"system_status\":false}");
        require(selectedMemory.memory.size() == 1, "Relevant memory was lost");
        const auto unrelated = selectContext(remembered,offeredMemory,
            "{\"context\":\"conversation\",\"sources\":[],\"history\":[],\"memory\":[],\"system_status\":false}");
        require(unrelated.memory.isEmpty(), "Unrelated memory reached generation");
        remembered.sources.push_back({QString(50000,'x'),{}});
        remembered.history.push_back({QStringLiteral("Поправка: меня зовут Лиза."),QStringLiteral("Принято.")});
        builder.build(remembered,[&](const PreparedContext& value,const QString& error){offeredMemory=value;memoryError=error;memoryLoop.quit();},ContextBuilder::Purpose::Selection);
        QTimer::singleShot(5000,&memoryLoop,&QEventLoop::quit);memoryLoop.exec();
        require(memoryError.isEmpty() && offeredMemory.memoryItems==1 && offeredMemory.sourceCount==0 && offeredMemory.historyPairs==1,
                "Unrelated retrieval candidates evicted memory or recent corrections before selection");
        qInfo() << "Memory planning, context selection, streaming, budgets and cancellation passed";
        return 0;
    } catch (const std::exception& error) { qCritical() << error.what(); return 1; }
}
