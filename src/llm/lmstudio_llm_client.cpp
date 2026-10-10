#include "lmstudio_llm_client.hpp"
#include "context_selection.hpp"
#include "system_status_policy.hpp"
#include "completion_stream.hpp"
#include <memory>
#include <stdexcept>

#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <QByteArray>
#include <QTimer>
#include <QDebug>
#include <QNetworkProxy>
#include <QFile>

LMStudioLLMClient::LMStudioLLMClient(const ModelApiConfig& config, QObject* parent) :
    ILLMClient(parent), config_(config) {
    manager_ = new QNetworkAccessManager(this);
    manager_->setProxy(QNetworkProxy::NoProxy);
    contextBuilder_ = new ContextBuilder(config, this);
}

void LMStudioLLMClient::cancelRequests()
{
    ++version_;
    contextBuilder_->cancel();
    preparedReady_ = false;
    for (auto* reply : manager_->findChildren<QNetworkReply*>()) {
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
}

void LMStudioLLMClient::prepare(const GenerationRequest& original) {
    cancelRequests();
    auto input = original;
    input.statusAccess = SystemStatusAccess::Unresolved;
    const auto request = contextWithoutUnrequestedStatus(input);
    if (request.sources.empty() && request.history.empty() && request.memory.isEmpty() && request.systemStatus.isEmpty()) {
        prepareAnswer(request);
        return;
    }
    contextBuilder_->build(request, [this, request](const PreparedContext& offered, const QString& error) {
        if (!error.isEmpty()) { emit errorOccurred(error); return; }
        QJsonObject body{{"model", config_.modelName}, {"messages", offered.messages}, {"stream", false},
                         {"max_tokens", offered.maxAnswerTokens}, {"temperature", 0},
                         {"chat_template_kwargs", QJsonObject{{"enable_thinking", false}}},
                         {"response_format", QJsonObject{{"type", "json_object"}, {"schema", contextSelectionSchema()}}}};
        complete(body, [this, request, offered](const QString& answer, const QString& error) {
            if (!error.isEmpty()) { emit errorOccurred(QStringLiteral("Не удалось отобрать контекст: ") + error); return; }
            if (qEnvironmentVariableIsSet("LOCAL_ASSISTANT_CONTEXT_DEBUG")) qInfo().noquote() << "Context selection:" << answer;
            try { prepareAnswer(selectContext(request, offered, answer)); }
            catch (const std::exception& e) {
                emit errorOccurred(QStringLiteral("Некорректный отбор контекста: ") + QString::fromUtf8(e.what()));
            }
        });
    }, ContextBuilder::Purpose::Selection);
}

void LMStudioLLMClient::planConversation(const QString& question, const QJsonObject& state, int contextSize, bool followup,
                                       std::function<void(const QJsonObject&, const QString&)> callback) {
    cancelRequests();
    if (state.value("compactThrough").toDouble() <= 0 && !followup) { callback({}, {}); return; }
    const auto messages = conversationPlanMessages(question, state, followup);
    contextBuilder_->countMessages(messages, [this,state,messages,contextSize,followup,callback,question](int tokens, const QString& error) {
        if (!error.isEmpty()) { callback({},error); return; }
        if (tokens + 512 + ContextBuilder::safetyTokens > contextSize) {
            callback({},QStringLiteral("Память: пакет не помещается в окно; сохранена исходная история")); return;
        }
        QJsonObject body{{"model",config_.modelName},{"messages",messages},{"stream",false},{"max_tokens",512},{"temperature",0},
            {"chat_template_kwargs",QJsonObject{{"enable_thinking",false}}},
            {"response_format",QJsonObject{{"type","json_object"},{"schema",conversationPlanSchema()}}}};
        complete(body, [state,followup,callback,question](const QString& answer, const QString& failure) {
            if (!failure.isEmpty()) { callback({},failure); return; }
            if (qEnvironmentVariableIsSet("LOCAL_ASSISTANT_CONTEXT_DEBUG")) qInfo().noquote() << "Conversation plan:" << answer;
            try { callback(validateConversationPlan(answer,state,followup,question), {}); }
            catch (const std::exception& e) { callback({},QString::fromUtf8(e.what())); }
        }, 60000);
    });
}

void LMStudioLLMClient::prepareAnswer(const GenerationRequest& request) {
    const bool hasSystemStatus = !request.systemStatus.isEmpty();
    contextBuilder_->build(request, [this, hasSystemStatus](const PreparedContext& context, const QString& error) {
        if (!error.isEmpty()) { emit errorOccurred(error); return; }
        prepared_ = context; preparedReady_ = true;
        emit contextPrepared(context.sourcesSnapshot,
            QStringLiteral("Контекст: текст %1, резерв изображений %2, ответ до %3, запас %4 / окно %5; история: %6 пар; источники: %7; MCP: %8; память: %9 выдержек")
                .arg(context.textTokens).arg(context.imageReserve).arg(context.maxAnswerTokens)
                .arg(ContextBuilder::safetyTokens).arg(context.contextSize).arg(context.historyPairs).arg(context.sourceCount)
            .arg(hasSystemStatus ? QStringLiteral("да") : QStringLiteral("нет")).arg(context.memoryItems), context.sourceReferences);
    });
}

void LMStudioLLMClient::generatePrepared() {
    if (!preparedReady_) { emit errorOccurred(QStringLiteral("Контекст запроса не подготовлен")); return; }
    preparedReady_ = false;
    QJsonObject body;
    body["model"] = config_.modelName;
    body["stream"] = true;
    body["messages"] = prepared_.messages;
    body["max_tokens"] = prepared_.maxAnswerTokens;
    // Use Qwen3-VL's presence penalties against loops, but keep text temperature
    // low: the more creative text preset also repeated irrelevant old replies.
    const bool vision = prepared_.imageReserve > 0;
    body["temperature"] = vision ? 0.7 : 0.2;
    body["top_p"] = vision ? 0.8 : 1.0;
    body["top_k"] = vision ? 20 : 40;
    body["min_p"] = 0.0;
    body["repeat_penalty"] = 1.0;
    body["presence_penalty"] = vision ? 1.5 : 2.0;
    body["chat_template_kwargs"] = QJsonObject{{"enable_thinking", false}};

    const bool systemStatusRequested = prepared_.systemStatusRequested;
    const auto references = prepared_.sourcesSnapshot;
    streamAnswer(body, [this, systemStatusRequested, references](const QString& answer, const QString& error) {
        if (!error.isEmpty()) emit errorOccurred(error);
        else {
            const auto visibleAnswer = systemStatusRequested ? answer : withoutSystemStatus(answer, references);
            if (visibleAnswer.trimmed().isEmpty())
                emit errorOccurred(QStringLiteral("Модель вернула только не запрошенные показатели компьютера. Повторите вопрос."));
            else emit answerReady(visibleAnswer);
        }
    });
}

void LMStudioLLMClient::streamAnswer(const QJsonObject& body, CompletionCallback callback) {
    const auto version = version_;
    const bool statusRequested = prepared_.systemStatusRequested;
    const auto references = prepared_.sourcesSnapshot;
    QNetworkRequest request(config_.baseUrl.resolved(QUrl("chat/completions")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "text/event-stream");
    auto* reply = manager_->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    auto stream = std::make_shared<CompletionStream>();
    auto lastPreview = std::make_shared<QString>();
    auto timedOut = std::make_shared<bool>(false);
    auto* deadline = new QTimer(reply); deadline->setSingleShot(true);
    auto* idle = new QTimer(reply); idle->setSingleShot(true);
    auto* updates = new QTimer(reply); updates->setSingleShot(true); updates->setInterval(100);
    const auto publish = [this, version, stream, lastPreview, statusRequested, references]() {
        if (version != version_) return;
        const auto preview = streamPreview(stream->text(), statusRequested, references);
        if (preview != *lastPreview && !preview.isEmpty()) { *lastPreview = preview; emit answerProgress(preview); }
    };
    connect(updates, &QTimer::timeout, this, publish);
    const auto receive = [reply, stream, idle, updates]() {
        if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) return;
        const auto bytes = reply->readAll();
        if (bytes.isEmpty()) return;
        idle->start(120000);
        stream->feed(bytes);
        if (!updates->isActive()) updates->start();
        if (!stream->error().isEmpty()) reply->abort();
    };
    connect(reply, &QNetworkReply::readyRead, this, receive);
    for (auto* timer : {deadline, idle}) connect(timer, &QTimer::timeout, this, [reply, timedOut]() {
        *timedOut = true; reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, stream, version, callback, deadline, idle, updates, receive, publish, timedOut]() {
        deadline->stop(); idle->stop(); updates->stop(); reply->deleteLater();
        if (version != version_) return;
        receive(); idle->stop(); updates->stop(); publish();
        if (version != version_) return; // A progress consumer may cancel or start another request.
        QString error = stream->error();
        if (*timedOut) error = QStringLiteral("Превышено время ожидания ответа модели");
        if (error.isEmpty() && reply->error() != QNetworkReply::NoError) error = reply->errorString();
        if (error.isEmpty() && !stream->complete()) error = QStringLiteral("Соединение прервано до завершения ответа модели");
        if (error.isEmpty() && stream->text().trimmed().isEmpty()) error = QStringLiteral("Модель вернула пустой ответ");
        if (!error.isEmpty()) { callback({}, error); return; }
        auto answer = stream->text();
        if (stream->finishReason() == "length")
            answer += QStringLiteral("\n\n---\n**Ответ не завершён:** достигнут лимит генерации. Не все запрошенные пункты могли попасть в ответ.");
        callback(answer, {});
    });
    idle->start(300000); // Prompt evaluation can precede the first event by several minutes on CPU.
    deadline->start(qBound(300000, body.value("max_tokens").toInt(1500) * 250, 1800000));
}

void LMStudioLLMClient::complete(const QJsonObject& body, CompletionCallback callback, int timeoutOverride) {
    const auto version = version_;
    const bool structured = body.contains("response_format");

    const QUrl url = config_.baseUrl.resolved(QUrl("chat/completions"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QByteArray bytes = QJsonDocument(body).toJson();
    auto* reply = manager_->post(request, bytes);
    auto* timer = new QTimer(reply);
    timer->setSingleShot(true);

    connect(timer, &QTimer::timeout,
            reply, &QNetworkReply::abort);

    connect(reply, &QNetworkReply::finished,
            timer, &QTimer::stop);

    connect(reply, &QNetworkReply::finished,
            this, [this, reply, version, structured, callback](){
        reply->deleteLater();
        if (version != version_) return;
        if (reply->error() != QNetworkReply::NoError) {
            callback({}, reply->errorString());
            return;
        } else {
            const QByteArray response = reply->readAll();

            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(response, &parseError);
            if (parseError.error != QJsonParseError::NoError) {
                callback({}, parseError.errorString());
                return;
            }

            if (!doc.isObject()) {
                callback({}, "Expected a JSON object");
                return;
            }

            const QJsonObject root = doc.object();
            const QJsonValue dataValue = root.value("choices");

            if (!dataValue.isArray()) {
                callback({}, "Data must be an array");
                return;
            }

            const QJsonArray data = dataValue.toArray();

            if (data.size() != 1 || !data.at(0).isObject()) {
                callback({}, "Expected one completion result");
                return;
            }
            const QJsonObject item = data.at(0).toObject();
            if (structured && item.value("finish_reason") == "length") {
                callback({}, QStringLiteral("Модель достигла ограничения длины ответа"));
                return;
            }
            const QJsonValue messageValue = item.value("message");

            if (!messageValue.isObject()) {
                callback({}, "Message value must be an object");
                return;
            }

             const QJsonObject messageData = messageValue.toObject();

             const QJsonValue contentValue = messageData.value("content");
             if (!contentValue.isString()) {
                 callback({}, "Content value must be a string");
                 return;
             }

             QString answer = contentValue.toString();
             if (answer.trimmed().isEmpty()) {
                 callback({}, "Answer is empty");
                 return;
             }

             if (!structured && item.value("finish_reason") == "length")
                 answer += QStringLiteral("\n\n---\n**Ответ не завершён:** достигнут лимит генерации. Не все запрошенные пункты могли попасть в ответ.");

             callback(answer, {});
       }
    });
    // A detailed technical answer on CPU/partial GPU offload can take longer than
    // five minutes. Keep a bound, but scale it with the configured output budget.
    const int timeoutMs = timeoutOverride > 0 ? timeoutOverride : structured ? 300000 : qBound(300000, body.value("max_tokens").toInt(1500) * 250, 1800000);
    timer->start(timeoutMs);

}
