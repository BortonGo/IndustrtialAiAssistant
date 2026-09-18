#pragma once

#include <QProcess>
#include <QString>
#include <QByteArray>
#include <QTimer>

class MCPClient final : public QObject {
    Q_OBJECT

    QProcess* process_ = nullptr;
    QByteArray outputBuffer_;

    QTimer* pollTimer_ = nullptr;
    QTimer* requestTimeoutTimer_ = nullptr;

    bool systemStatusPending_ = false;
    bool pollingPausedByTimeout_ = false;

public:
    explicit MCPClient(QObject* parent = nullptr);

    void start();

    void stop();

    ~MCPClient() override;

signals:
    void errorOccurred(const QString& message);
    void systemStatusReady(double cpuPercent, double memoryPercent);

private:
    void sendInitialize();
    void sendInitialized();
    void requestTools();
    void requestSystemStatus();
};
