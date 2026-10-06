#include "docx_text_extractor.hpp"

#include <QProcess>
#include <QTimer>

DocxTextExtractor::DocxTextExtractor(const QString& pythonPath, const QString& scriptPath,
                                   QObject* parent) :
    QObject(parent), pythonPath_(pythonPath), scriptPath_(scriptPath) {
    process_ = new QProcess(this);
    timer_ = new QTimer(this);
    timer_->setInterval(30000);
    timer_->setSingleShot(true);

    connect(process_, &QProcess::readyReadStandardOutput,
            this, [this]() {
        outputBuffer_.append(process_->readAllStandardOutput());
    });

    connect(process_, &QProcess::readyReadStandardError,
            this, [this]() {
        errorBuffer_.append(process_->readAllStandardError());
    });

    connect(process_, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(
                &QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
        timer_->stop();
        outputBuffer_.append(process_->readAllStandardOutput());
        errorBuffer_.append(process_->readAllStandardError());

        const QString text = QString::fromUtf8(outputBuffer_);
        const QString errorText = QString::fromUtf8(errorBuffer_).trimmed();
        const bool wasTimedOut = timeOut_;
        outputBuffer_.clear();
        errorBuffer_.clear();
        timeOut_ = false;

        if (wasTimedOut) {
            emit errorOccurred("DOCX extraction timed out");
            return;
        }
        if (exitStatus != QProcess::NormalExit || exitCode != 0) {
            emit errorOccurred(errorText.isEmpty() ? "DOCX extraction failed" : errorText);
            return;
        }
        if (text.trimmed().isEmpty()) {
            emit errorOccurred("No text found in DOCX");
            return;
        }
        emit textReady(text);
    });

    connect(process_, &QProcess::errorOccurred,
            this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            timer_->stop();
            outputBuffer_.clear();
            errorBuffer_.clear();
            timeOut_ = false;
            emit errorOccurred(process_->errorString());
        }
    });

    connect(timer_, &QTimer::timeout,
            this, [this]() {
        timeOut_ = true;
        process_->kill();
    });
}

void DocxTextExtractor::extract(const QString& path) {
    if (process_->state() != QProcess::NotRunning) {
        emit errorOccurred("DOCX extraction is already running");
        return;
    }
    outputBuffer_.clear();
    errorBuffer_.clear();
    timeOut_ = false;

    QStringList arguments;
    arguments << "-u" << scriptPath_ << path;
    timer_->start();
    process_->start(pythonPath_, arguments);
}
