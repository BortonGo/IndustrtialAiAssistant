#include "pdf_document_reader.hpp"

#include <QFileInfo>

#include <stdexcept>

PdfDocumentReader::PdfDocumentReader(const QString& executablePath,
                           qint64 maxFileSize,QObject* parent) :
    IDocumentReader(parent), maxFileSize_(maxFileSize) {
    if (maxFileSize <= 0) {
        throw std::invalid_argument("Maximum document size must be > 0");
    }
    extractor_ = new PdfTextExtractor(executablePath, this);

    connect(extractor_, &PdfTextExtractor::textReady,
            this, [this](const QString& text) {
        const QFileInfo fileInfo(pendingPath_);
        Document document;
        document.id = fileInfo.absoluteFilePath();
        document.sourcePath = fileInfo.absoluteFilePath();
        document.text = text;
        pendingPath_.clear();
        busy_ = false;
        emit documentReady(document);
    });

    connect(extractor_, &PdfTextExtractor::errorOccurred,
            this, [this](const QString& message) {
        pendingPath_.clear();
        busy_ = false;
        emit errorOccurred(message);
    });
}

void PdfDocumentReader::load(const QString& path) {
    if (busy_) {
        emit errorOccurred("PDF extraction is already running");
        return;
    }
    const QFileInfo fileInfo(path);
    if (path.trimmed().isEmpty()) {
        emit errorOccurred("PDF file path is empty");
        return;
    }
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        emit errorOccurred("PDF file does not exist or is not a regular file");
        return;
    }
    if (!fileInfo.isReadable()) {
        emit errorOccurred("PDF file is not readable");
        return;
    }
    if (fileInfo.size() > maxFileSize_) {
        emit errorOccurred("PDF file exceeds size limit");
        return;
    }
    pendingPath_ = fileInfo.absoluteFilePath();
    busy_ = true;
    extractor_->extract(pendingPath_);
}
