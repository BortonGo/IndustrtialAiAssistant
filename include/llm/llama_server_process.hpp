#pragma once

#include "llama_server_config.hpp"
#include <QObject>
#include <QByteArray>

class QProcess;
class QTimer;
class QNetworkAccessManager;
class QNetworkReply;

class LlamaServerProcess final : public QObject {
    Q_OBJECT

public:
    explicit LlamaServerProcess(const LlamaServerConfig& config,QObject* parent = nullptr);

    ~LlamaServerProcess() override;

    void start();
    void stop();
    bool isReady() const;
    bool isStopped() const;
    QString statusText() const;

signals:
    void ready();
    void stopped();
    void errorOccurred(const QString& message);
    void statusChanged(const QString& text);
    void logMessage(const QString& text);

private:
    enum class State {
        Stopped,
        Starting,
        Ready,
        Stopping
    };

    LlamaServerConfig config_;
    QProcess* process_ = nullptr;
    QTimer* killTimer_ = nullptr;
    QTimer* healthTimer_ = nullptr;
    QTimer* startupTimer_ = nullptr;
    State state_ = State::Stopped;
#ifdef Q_OS_WIN
    void* jobHandle_ = nullptr;
#endif

    QNetworkAccessManager* networkManager_ = nullptr;
    QNetworkReply* healthReply_ = nullptr;
    QString statusText_ = QStringLiteral("Остановлен");
    QString lastError_;
    QByteArray logBuffer_;
    QString logTail_;

    void checkHealth();
    void stopHealthChecks();
    void setStatus(const QString& text);
    void fail(const QString& message);
    void readLog(bool flush = false);
};
