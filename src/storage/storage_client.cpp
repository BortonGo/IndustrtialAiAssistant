#include "storage/storage_client.hpp"
#include <QProcess>
#include <QTimer>
#include <QDir>
#include <QSettings>
#include <QJsonDocument>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

StorageClient::StorageClient(const QString& root, const QString& model, QObject* parent)
    : QObject(parent), root_(root), model_(model), process_(new QProcess(this)), timer_(new QTimer(this)) {
    timer_->setSingleShot(true);
    timer_->setInterval(120000);
#ifdef Q_OS_WIN
    process_->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) { args->flags |= CREATE_NO_WINDOW; });
#endif
    connect(process_, &QProcess::readyReadStandardOutput, this, &StorageClient::read);
    connect(process_, &QProcess::readyReadStandardError, this, [this]() { errors_ = (errors_ + process_->readAllStandardError()).right(4000); });
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) { fail(process_->errorString()); emit stopped(); }
    });
    connect(process_, static_cast<void(QProcess::*)(int,QProcess::ExitStatus)>(&QProcess::finished), this,
            [this](int, QProcess::ExitStatus) {
        read(); timer_->stop(); ready_ = false;
        if (!stopping_) fail(QStringLiteral("Процесс БД завершился: ") + QString::fromUtf8(errors_));
        else emit statusChanged(QStringLiteral("Остановлена"));
        emit stopped();
    });
    connect(timer_, &QTimer::timeout, this, [this]() { fail(QStringLiteral("Таймаут операции БД")); stop(); });
}

StorageClient::~StorageClient() {
    process_->disconnect(this);
    process_->closeWriteChannel();
    if (process_->state() != QProcess::NotRunning && !process_->waitForFinished(15000)) {
        process_->kill(); process_->waitForFinished(1000);
    }
}

void StorageClient::start() {
    if (process_->state() != QProcess::NotRunning) return;
    stopping_ = false;
    QSettings settings(QDir(root_).filePath("local-assistant.ini"), QSettings::IniFormat);
    const QDir root(root_);
    const auto overrideDirectory = QString::fromLocal8Bit(qgetenv("LOCAL_ASSISTANT_STORAGE_DIR"));
    const auto overridePort = QString::fromLocal8Bit(qgetenv("LOCAL_ASSISTANT_STORAGE_PORT"));
    const auto data = overrideDirectory.isEmpty() ? settings.value("storage/dataDirectory", "storage-data").toString() : overrideDirectory;
    QStringList args{"-u", root.filePath("storage_tools/server.py"), "--root", root_,
                     "--data", QDir::isAbsolutePath(data) ? data : root.filePath(data),
                     "--model", model_, "--port", overridePort.isEmpty() ? settings.value("storage/port", 55432).toString() : overridePort};
    emit statusChanged(QStringLiteral("Запуск PostgreSQL…"));
    timer_->start();
    process_->start(root.filePath("storage_tools/.venv/Scripts/python.exe"), args);
}

bool StorageClient::isStopped() const { return process_->state() == QProcess::NotRunning; }

void StorageClient::stop() {
    if (stopping_) return;
    stopping_ = true; ready_ = false;
    process_->closeWriteChannel(); // Worker drains queued writes, then pg_ctl performs a clean shutdown.
    QTimer::singleShot(15000, this, [this]() { if (!isStopped()) process_->kill(); });
}

void StorageClient::request(const QString& method, const QJsonObject& params, Callback callback) {
    if (!ready_ || stopping_) {
        QTimer::singleShot(0, this, [callback]() { callback({}, QStringLiteral("БД недоступна")); });
        return;
    }
    const int id = ++sequence_;
    callbacks_.insert(id, std::move(callback));
    process_->write(QJsonDocument(QJsonObject{{"id",id},{"method",method},{"params",params}}).toJson(QJsonDocument::Compact) + '\n');
    timer_->start();
}

void StorageClient::fail(const QString& error) {
    ready_ = false;
    timer_->stop();
    auto pending = std::move(callbacks_);
    callbacks_.clear();
    emit statusChanged(QStringLiteral("Ошибка: ") + error);
    for (const auto& callback : pending) callback({}, error);
    emit unavailable(error);
}

void StorageClient::read() {
    buffer_ += process_->readAllStandardOutput();
    if (buffer_.size() > 256 * 1024 * 1024) { fail(QStringLiteral("Ответ БД превышает 256 МиБ")); stop(); return; }
    int end;
    while ((end = buffer_.indexOf('\n')) >= 0) {
        QJsonParseError error;
        const auto value = QJsonDocument::fromJson(buffer_.left(end), &error);
        buffer_.remove(0, end + 1);
        if (error.error != QJsonParseError::NoError || !value.isObject()) { fail(QStringLiteral("Неверный ответ БД")); stop(); return; }
        const auto object = value.object();
        if (object.contains("fatal")) { fail(object.value("fatal").toString()); return; }
        if (object.value("ready").toBool()) {
            timer_->stop(); ready_ = true;
            emit statusChanged(QStringLiteral("Готова · PostgreSQL")); emit ready(); continue;
        }
        const int id = object.value("id").toInt();
        if (!callbacks_.contains(id)) continue;
        auto callback = callbacks_.take(id);
        if (callbacks_.isEmpty()) timer_->stop(); else timer_->start();
        callback(object.value("result").toObject(), object.value("error").toString());
    }
}
