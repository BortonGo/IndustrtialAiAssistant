#pragma once

#include <QByteArray>
#include <QString>

// Incremental OpenAI-compatible SSE decoder. TCP boundaries may split UTF-8 or JSON.
class CompletionStream {
    QByteArray pending_, event_;
    QString text_, error_, finishReason_;
    bool done_ = false;
    void dispatch();
public:
    void feed(const QByteArray& bytes);
    QString error() const { return error_; }
    QString text() const { return text_; }
    QString finishReason() const { return finishReason_; }
    bool complete() const { return done_ && !finishReason_.isEmpty() && error_.isEmpty(); }
};

// Only expose completed sentences/lines to the telemetry filter: an unfinished
// "CPU ..." sentence must not briefly appear before its numbers arrive.
QString streamPreview(const QString& text, bool systemStatusRequested, const QString& references);
