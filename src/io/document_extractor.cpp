#include "document_extractor.hpp"
#include <QProcess>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

DocumentExtractor::DocumentExtractor(const DocumentExtractionConfig& config, QObject* parent)
    : QObject(parent), config_(config)
{
    process_ = new QProcess(this);
    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    timer_->setInterval(config_.inactivityTimeoutMs);
#ifdef Q_OS_WIN
    process_->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags |= CREATE_NO_WINDOW;
    });
#endif
    connect(process_, &QProcess::started, this, [this]() {
#ifdef Q_OS_WIN
        HANDLE child = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE,
                                   static_cast<DWORD>(process_->processId()));
        const bool assigned = child && job_ && AssignProcessToJobObject(static_cast<HANDLE>(job_), child);
        if (child) CloseHandle(child);
        if (!assigned) {
            failure_ = QStringLiteral("Не удалось изолировать обработчик документов (Windows Job Object)");
            killWorker();
        }
#endif
    });
    connect(process_, &QProcess::readyReadStandardOutput, this, &DocumentExtractor::readOutput);
    connect(process_, &QProcess::readyReadStandardError, this, &DocumentExtractor::readErrors);
    connect(timer_, &QTimer::timeout, this, [this]() {
        failure_ = QStringLiteral("Обработчик документа не отвечает: превышен таймаут");
        killWorker();
    });
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || !active_) return;
        active_ = false;
        timer_->stop();
        closeJob();
        emit errorOccurred(QStringLiteral("Не удалось запустить Python: ") + process_->errorString());
    });
    connect(process_, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus status) {
        if (!active_) return;
        readOutput();
        readErrors();
        timer_->stop();
        active_ = false;
        closeJob();
        if (!failure_.isEmpty()) { emit errorOccurred(failure_); return; }
        if (code != 0 || status != QProcess::NormalExit) {
            emit errorOccurred(QStringLiteral("Ошибка обработки документа: ") +
                               QString::fromUtf8(errorTail_ + stderrBuffer_).right(4000));
            return;
        }
        Document document;
        QString error;
        if (!parseDocumentContent(output_, document, error)) { emit errorOccurred(error); return; }
        output_.clear();
        emit contentReady(document);
    });
}

DocumentExtractor::~DocumentExtractor()
{
    process_->disconnect(this);
    killWorker();
    process_->waitForFinished(1000);
    closeJob();
}

void DocumentExtractor::closeJob()
{
#ifdef Q_OS_WIN
    if (job_) CloseHandle(static_cast<HANDLE>(job_));
#endif
    job_ = nullptr;
}

void DocumentExtractor::killWorker()
{
    closeJob(); // Also terminates Poppler / converter children.
    if (process_->state() != QProcess::NotRunning) process_->kill();
}

void DocumentExtractor::cancel()
{
    if (!active_) return;
    failure_ = QStringLiteral("Обработка документа отменена");
    killWorker();
}

void DocumentExtractor::readOutput()
{
    const auto bytes = process_->readAllStandardOutput();
    if (output_.size() + bytes.size() > 64 * 1024 * 1024) {
        failure_ = QStringLiteral("Извлечённый документ превышает лимит 64 МиБ");
        killWorker();
        return;
    }
    output_.append(bytes);
}

void DocumentExtractor::readErrors()
{
    stderrBuffer_ += process_->readAllStandardError();
    int end;
    while ((end = stderrBuffer_.indexOf('\n')) >= 0) {
        const auto line = stderrBuffer_.left(end).trimmed();
        stderrBuffer_.remove(0, end + 1);
        if (line.startsWith("@@DOCUMENT@@")) {
            const auto message = QJsonDocument::fromJson(line.mid(12)).object().value("message").toString();
            if (!message.isEmpty()) {
                timer_->start();
                emit progressChanged(message);
            }
        } else errorTail_ = (errorTail_ + line + '\n').right(4000);
    }
    if (stderrBuffer_.size() > 8192) stderrBuffer_ = stderrBuffer_.right(8192);
}

void DocumentExtractor::extract(const QString& path)
{
    if (active_) { emit errorOccurred("Document extraction is already running"); return; }
    output_.clear(); stderrBuffer_.clear(); errorTail_.clear(); failure_.clear();
    const auto cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cache.isEmpty()) { emit errorOccurred("Cannot determine document cache directory"); return; }
#ifdef Q_OS_WIN
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        if (job) CloseHandle(job);
        emit errorOccurred("Cannot create document worker Job Object");
        return;
    }
    job_ = job;
#endif
    QStringList args;
    args << "-u" << config_.scriptPath << path << "--images-dir" << QDir(cache).filePath("document-images")
         << "--poppler-dir" << config_.popplerDirectory;
    if (!config_.visionUrl.isEmpty()) args << "--vision-url" << config_.visionUrl
                                        << "--vision-profile" << config_.visionProfile;
    if (!config_.sofficePath.isEmpty()) args << "--soffice" << config_.sofficePath;
    active_ = true;
    timer_->start();
    process_->start(config_.pythonPath, args);
}
