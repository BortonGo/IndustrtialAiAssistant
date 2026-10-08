#include "completion_stream.hpp"
#include "system_status_policy.hpp"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>

void CompletionStream::feed(const QByteArray& bytes) {
    if (!error_.isEmpty()) return;
    pending_ += bytes;
    int end;
    while ((end = pending_.indexOf('\n')) >= 0) {
        auto line = pending_.left(end); pending_.remove(0, end + 1);
        if (line.endsWith('\r')) line.chop(1);
        if (line.isEmpty()) dispatch();
        else if (line.startsWith("data:")) {
            auto data = line.mid(5); if (data.startsWith(' ')) data.remove(0, 1);
            event_ += data + '\n';
        }
        if (event_.size() > 1024 * 1024 || text_.size() > 2 * 1024 * 1024)
            error_ = QStringLiteral("Слишком большой поток ответа модели");
        if (!error_.isEmpty()) return;
    }
    if (pending_.size() > 1024 * 1024) error_ = QStringLiteral("Слишком большое событие в потоке ответа");
}

void CompletionStream::dispatch() {
    if (event_.isEmpty()) return;
    const auto event = event_.trimmed(); event_.clear();
    if (done_) { error_ = QStringLiteral("Данные после завершения потока ответа"); return; }
    if (event == "[DONE]") { done_ = true; return; }
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(event, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        error_ = QStringLiteral("Повреждённое JSON-событие в потоке ответа"); return;
    }
    const auto root = document.object();
    if (root.contains("error")) {
        error_ = QStringLiteral("Ошибка сервера: ") + root.value("error").toObject().value("message").toString(); return;
    }
    if (!root.value("choices").isArray()) { error_ = QStringLiteral("В потоке ответа отсутствует choices"); return; }
    const auto choices = root.value("choices").toArray();
    if (choices.isEmpty()) return; // Optional usage-only event.
    if (choices.size() != 1 || !choices[0].isObject()) { error_ = QStringLiteral("Ожидался один поток ответа"); return; }
    const auto choice = choices[0].toObject();
    if (choice.value("index").toInt(-1) != 0 || !choice.value("delta").isObject() || !finishReason_.isEmpty()) {
        error_ = QStringLiteral("Неверная последовательность событий ответа"); return;
    }
    const auto delta = choice.value("delta").toObject();
    const auto content = delta.value("content");
    if ((!content.isUndefined() && !content.isNull() && !content.isString()) || delta.contains("tool_calls")) {
        error_ = QStringLiteral("Неподдерживаемый формат потокового ответа"); return;
    }
    text_ += content.toString();
    const auto reason = choice.value("finish_reason");
    if (!reason.isUndefined() && !reason.isNull()) {
        finishReason_ = reason.toString();
        if (finishReason_ != "stop" && finishReason_ != "length")
            error_ = QStringLiteral("Модель прервала ответ: ") + finishReason_;
    }
}

QString streamPreview(const QString& text, bool systemStatusRequested, const QString& references) {
    if (systemStatusRequested) return text;
    static const QRegularExpression boundary(QStringLiteral("[.!?;]\\s+|\\n"));
    auto matches = boundary.globalMatch(text);
    int end = 0;
    while (matches.hasNext()) end = matches.next().capturedEnd();
    return withoutSystemStatus(text.left(end), references);
}
