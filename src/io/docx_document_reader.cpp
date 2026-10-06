#include "docx_document_reader.hpp"
#include "docx_text_extractor.hpp"

#include <QFileInfo>

#include <stdexcept>

DocxDocumentReader::DocxDocumentReader(const QString& pythonPath, const QString& scriptPath,
                                     qint64 maxFileSize, QObject* parent) :
    IDocumentReader(parent), maxFileSize_(maxFileSize) {
    if (maxFileSize <= 0) {
        throw std::invalid_argument("Maximum document size must be > 0");
    }
    extractor_ = new DocxTextExtractor(pythonPath, scriptPath, this);

    connect(extractor_, &DocxTextExtractor::textReady,
            this, [this](const QString& text) {
        Document document;
        document.id = pendingPath_;
        document.sourcePath = pendingPath_;
        document.text = text;
        pendingPath_.clear();
        busy_ = false;
        emit documentReady(document);
    });

    connect(extractor_, &DocxTextExtractor::errorOccurred,
            this, [this](const QString& message) {
        pendingPath_.clear();
        busy_ = false;
        emit errorOccurred(message);
    });
}

void DocxDocumentReader::load(const QString& path) {
    if (busy_) {
        emit errorOccurred("DOCX extraction is already running");
        return;
    }
    const QFileInfo fileInfo(path);
    if (path.trimmed().isEmpty()) {
        emit errorOccurred("DOCX file path is empty");
        return;
    }
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        emit errorOccurred("DOCX file does not exist or is not a regular file");
        return;
    }
    if (!fileInfo.isReadable()) {
        emit errorOccurred("DOCX file is not readable");
        return;
    }
    if (fileInfo.size() > maxFileSize_) {
        emit errorOccurred("DOCX file exceeds size limit");
        return;
    }
    pendingPath_ = fileInfo.absoluteFilePath();
    busy_ = true;
    extractor_->extract(pendingPath_);
}
