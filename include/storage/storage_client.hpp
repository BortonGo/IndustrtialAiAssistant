#pragma once
#include <QObject>
#include <QJsonObject>
#include <QMap>
#include <functional>

class QProcess;
class QTimer;

class StorageClient final : public QObject {
    Q_OBJECT
public:
    using Callback = std::function<void(const QJsonObject&, const QString&)>;
    explicit StorageClient(const QString& root, const QString& model, QObject* parent = nullptr);
    ~StorageClient() override;
    void start();
    void stop();
    bool isReady() const { return ready_; }
    bool isStopped() const;
    void request(const QString& method, const QJsonObject& params, Callback callback);
signals:
    void ready();
    void stopped();
    void statusChanged(const QString& text);
    void unavailable(const QString& error);
private:
    QString root_, model_;
    QProcess* process_;
    QTimer* timer_;
    QByteArray buffer_, errors_;
    QMap<int, Callback> callbacks_;
    int sequence_ = 0;
    bool ready_ = false, stopping_ = false;
    void read();
    void fail(const QString& error);
};
