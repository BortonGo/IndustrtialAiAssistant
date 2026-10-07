#include "llama_server_process.hpp"

#include <QProcess>
#include <QFileInfo>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QNetworkProxy>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QHostAddress>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

LlamaServerProcess::LlamaServerProcess(const LlamaServerConfig& config, QObject* parent)
    : QObject(parent), config_(config)
{
    process_ = new QProcess(this);
    process_->setProcessChannelMode(QProcess::MergedChannels);
    process_->setWorkingDirectory(QFileInfo(config_.executablePath).absolutePath());
#ifdef Q_OS_WIN
    process_->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags |= CREATE_NO_WINDOW;
    });
#endif
    networkManager_ = new QNetworkAccessManager(this);
    networkManager_->setProxy(QNetworkProxy::NoProxy);
    killTimer_ = new QTimer(this);
    killTimer_->setSingleShot(true);
    killTimer_->setInterval(1000);
    healthTimer_ = new QTimer(this);
    healthTimer_->setInterval(500);
    startupTimer_ = new QTimer(this);
    startupTimer_->setSingleShot(true);
    startupTimer_->setInterval(config_.startupTimeoutMs);

    connect(killTimer_, &QTimer::timeout, this, [this]() {
        if (state_ == State::Stopping && process_->state() != QProcess::NotRunning) {
            process_->kill();
        }
    });
    connect(healthTimer_, &QTimer::timeout, this, &LlamaServerProcess::checkHealth);
    connect(startupTimer_, &QTimer::timeout, this, [this]() {
        if (state_ == State::Starting) {
            fail(QStringLiteral("Модель не загрузилась за %1 секунд")
                     .arg(config_.startupTimeoutMs / 1000));
        }
    });
    connect(process_, &QProcess::started, this, [this]() {
#ifdef Q_OS_WIN
        if (jobHandle_) {
            HANDLE childHandle = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE,
                                             FALSE, static_cast<DWORD>(process_->processId()));
            if (!childHandle) {
                emit logMessage(QStringLiteral("Не удалось открыть процесс для защиты от зависшего сервера: Windows %1")
                                    .arg(GetLastError()));
            } else {
                if (!AssignProcessToJobObject(static_cast<HANDLE>(jobHandle_), childHandle)) {
                    emit logMessage(QStringLiteral("Не удалось подключить процесс к Job Object: Windows %1. Остановка через QProcess остаётся активной")
                                        .arg(GetLastError()));
                }
                CloseHandle(childHandle);
            }
        }
#endif
        if (state_ == State::Stopping) {
            process_->terminate();
            return;
        }
        if (state_ != State::Starting) {
            return;
        }
        setStatus(QStringLiteral("Загрузка модели…"));
        healthTimer_->start();
        checkHealth();
    });
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            killTimer_->stop();
            state_ = State::Stopped;
            fail(QStringLiteral("Не удалось запустить сервер: %1")
                     .arg(process_->errorString()));
            emit stopped();
        }
    });
    connect(process_, &QProcess::readyReadStandardOutput, this, [this]() { readLog(); });
    connect(process_,
            static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
        readLog(true);
        killTimer_->stop();
        const bool expectedStop = state_ == State::Stopping;
        state_ = State::Stopped;
        stopHealthChecks();
        if (!expectedStop) {
            lastError_ = QStringLiteral("Сервер завершился: код %1 (%2)")
                             .arg(exitCode)
                             .arg(exitStatus == QProcess::NormalExit
                                      ? QStringLiteral("выход") : QStringLiteral("сбой"));
            setStatus(QStringLiteral("Ошибка: %1").arg(lastError_));
            emit errorOccurred(lastError_ + QStringLiteral("\n") + logTail_.right(2000));
        } else {
            setStatus(lastError_.isEmpty() ? QStringLiteral("Остановлен")
                                          : QStringLiteral("Ошибка: %1").arg(lastError_));
        }
        emit stopped();
    });
}

LlamaServerProcess::~LlamaServerProcess()
{
    process_->disconnect(this);
    stopHealthChecks();
    killTimer_->stop();
    if (process_->state() != QProcess::NotRunning) {
        process_->terminate();
        if (!process_->waitForFinished(1000)) {
            process_->kill();
            process_->waitForFinished(1000);
        }
    }
#ifdef Q_OS_WIN
    if (jobHandle_) {
        CloseHandle(static_cast<HANDLE>(jobHandle_));
        jobHandle_ = nullptr;
    }
#endif
}

bool LlamaServerProcess::isReady() const { return state_ == State::Ready; }
bool LlamaServerProcess::isStopped() const
{
    return state_ == State::Stopped && process_->state() == QProcess::NotRunning;
}
QString LlamaServerProcess::statusText() const { return statusText_; }

void LlamaServerProcess::setStatus(const QString& text)
{
    if (statusText_ == text) {
        return;
    }
    statusText_ = text;
    emit statusChanged(text);
    emit logMessage(text);
}

void LlamaServerProcess::fail(const QString& message)
{
    lastError_ = message;
    stopHealthChecks();
    stop();
    setStatus(QStringLiteral("Ошибка: %1").arg(message));
    emit errorOccurred(message);
}

void LlamaServerProcess::start()
{
    if (state_ != State::Stopped) {
        return;
    }
    lastError_.clear();
    logBuffer_.clear();
    logTail_.clear();
    if (!QFileInfo(config_.executablePath).isFile()) {
        fail(QStringLiteral("Не найден llama-server.exe: %1").arg(config_.executablePath));
        return;
    }
    if (config_.healthUrl.scheme() != QStringLiteral("http") ||
        (config_.healthUrl.host() != QStringLiteral("127.0.0.1") &&
         config_.healthUrl.host() != QStringLiteral("localhost")) ||
        config_.healthUrl.port() < 1 || config_.healthUrl.port() > 65535 ||
        config_.startupTimeoutMs <= 0) {
        fail(QStringLiteral("Неверные настройки адреса или таймаута сервера"));
        return;
    }
    const int modelArgument = config_.arguments.indexOf(QStringLiteral("--model"));
    if (modelArgument >= 0 &&
        (modelArgument + 1 >= config_.arguments.size() ||
         !QFileInfo(config_.arguments.at(modelArgument + 1)).isFile())) {
        fail(QStringLiteral("Не найден файл модели GGUF"));
        return;
    }
    // A manually started server must not be mistaken for this child's /health.
    QTcpServer portProbe;
    if (!portProbe.listen(QHostAddress::LocalHost, config_.healthUrl.port())) {
        fail(QStringLiteral("Порт %1 занят. Остановите сервер в терминале или измените порт")
                 .arg(config_.healthUrl.port()));
        return;
    }
    portProbe.close();
#ifdef Q_OS_WIN
    if (!jobHandle_) {
        // An unnamed, non-inherited handle protects only our own child process.
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (!job) {
            emit logMessage(QStringLiteral("Не удалось создать Job Object: Windows %1. Остановка через QProcess остаётся активной")
                                .arg(GetLastError()));
        } else {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                         &limits, sizeof(limits))) {
                const DWORD error = GetLastError();
                CloseHandle(job);
                emit logMessage(QStringLiteral("Не удалось настроить Job Object: Windows %1. Остановка через QProcess остаётся активной")
                                    .arg(error));
            } else {
                jobHandle_ = job;
            }
        }
    }
#endif
    state_ = State::Starting;
    setStatus(QStringLiteral("Запуск…"));
    startupTimer_->start();
    process_->start(config_.executablePath, config_.arguments);
}

void LlamaServerProcess::stop()
{
    if (state_ == State::Stopped || state_ == State::Stopping) {
        return;
    }
    state_ = State::Stopping;
    stopHealthChecks();
    setStatus(QStringLiteral("Остановка…"));
    if (process_->state() == QProcess::NotRunning) {
        state_ = State::Stopped;
        setStatus(QStringLiteral("Остановлен"));
        emit stopped();
        return;
    }
    killTimer_->start();
    process_->terminate();
}

void LlamaServerProcess::stopHealthChecks()
{
    healthTimer_->stop();
    startupTimer_->stop();
    if (healthReply_ != nullptr) {
        auto* reply = healthReply_;
        healthReply_ = nullptr;
        reply->abort();
    }
}

void LlamaServerProcess::checkHealth()
{
    if (state_ != State::Starting || process_->state() != QProcess::Running || healthReply_) {
        return;
    }
    auto* reply = networkManager_->get(QNetworkRequest(config_.healthUrl));
    healthReply_ = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (healthReply_ != reply) {
            return;
        }
        healthReply_ = nullptr;
        if (state_ != State::Starting || process_->state() != QProcess::Running) {
            return;
        }
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || httpStatus != 200) {
            return;
        }
        const auto document = QJsonDocument::fromJson(reply->readAll());
        if (!document.isObject() ||
            document.object().value(QStringLiteral("status")).toString() != QStringLiteral("ok")) {
            return;
        }
        state_ = State::Ready;
        stopHealthChecks();
        setStatus(QStringLiteral("Готов"));
        emit ready();
    });
    QTimer::singleShot(3000, reply, [reply]() {
        if (reply->isRunning()) {
            reply->abort();
        }
    });
}

void LlamaServerProcess::readLog(bool flush)
{
    logBuffer_.append(process_->readAllStandardOutput());
    int newline;
    while ((newline = logBuffer_.indexOf('\n')) >= 0) {
        const QString line = QString::fromUtf8(logBuffer_.left(newline)).trimmed();
        logBuffer_.remove(0, newline + 1);
        if (!line.isEmpty()) {
            logTail_ = (logTail_ + line + QLatin1Char('\n')).right(8000);
            emit logMessage(line);
        }
    }
    if ((flush || logBuffer_.size() > 16000) && !logBuffer_.isEmpty()) {
        const QString line = QString::fromUtf8(logBuffer_).trimmed();
        logBuffer_.clear();
        logTail_ = (logTail_ + line).right(8000);
        emit logMessage(line);
    }
}
