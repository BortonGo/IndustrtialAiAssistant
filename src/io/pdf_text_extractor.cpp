#include "pdf_text_extractor.hpp"

#include <QProcess>

PdfTextExtractor::PdfTextExtractor(const QString& executablePath, QObject* parent) :
    QObject(parent), executablePath_(executablePath) {
    process_ = new QProcess(this);

    connect(process_, &QProcess::readyReadStandardOutput,
            this, [this]() {
        outputBuffer_.append(process_->readAllStandardOutput());
    });

    connect(process_, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(
                &QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
        timer_->stop();

        if (timeOut_) {
            emit errorOccurred("PDF extraction timed out");
            return;
        }

        outputBuffer_.append(process_->readAllStandardOutput());

        if (exitStatus != QProcess::NormalExit || exitCode != 0) {
            emit errorOccurred("Extraction error");
            return;
        }

        const QString text = QString::fromUtf8(outputBuffer_);

        if (text.trimmed().isEmpty()) {
            emit errorOccurred("In pdf file not found extraction text");
            return;
        }

        emit textReady(text);
    });

    connect(process_, &QProcess::errorOccurred,
            this, [this]() {
        if (process_->error() == QProcess::FailedToStart) {
            timer_->stop();
            timeOut_ = false;
            emit errorOccurred(process_->errorString());
            return;
        }
    });

    timer_ = new QTimer(this);
    timer_->setInterval(30000);
    timer_->setSingleShot(true);

    connect(timer_, &QTimer::timeout,
            this, [this]() {
        timeOut_ = true;
        process_->kill();
    });
}

void PdfTextExtractor::extract(const QString& pdfPath) {
    if (process_->state() != QProcess::NotRunning) {
        emit errorOccurred("PDF extraction is already running");
        return;
    }
    outputBuffer_.clear();
    QStringList arguments;
    arguments << "-enc" << "UTF-8" << pdfPath << "-";
    timeOut_ = false;
    timer_->start();
    process_->start(executablePath_, arguments);
}
