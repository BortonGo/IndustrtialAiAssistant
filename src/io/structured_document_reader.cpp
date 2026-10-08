#include "structured_document_reader.hpp"
#include <QFileInfo>

StructuredDocumentReader::StructuredDocumentReader(const DocumentExtractionConfig& config, QObject* parent)
    : IDocumentReader(parent), extractor_(new DocumentExtractor(config, this)), maxFileSize_(config.maxFileSize)
{
    connect(extractor_, &DocumentExtractor::contentReady, this, [this](const Document& content) {
        Document document = content;
        document.id = document.sourcePath = pendingPath_;
        pendingPath_.clear();
        emit documentReady(document);
    });
    connect(extractor_, &DocumentExtractor::errorOccurred, this, [this](const QString& error) {
        pendingPath_.clear();
        emit errorOccurred(error);
    });
    connect(extractor_, &DocumentExtractor::progressChanged, this, &IDocumentReader::progressChanged);
}

void StructuredDocumentReader::load(const QString& path)
{
    if (!pendingPath_.isEmpty()) { emit errorOccurred("Document reader is busy"); return; }
    const QFileInfo info(path);
    if (!info.isFile() || !info.isReadable()) {
        emit errorOccurred(QStringLiteral("Документ не найден или недоступен: ") + path);
        return;
    }
    if (info.size() > maxFileSize_) {
        emit errorOccurred(QStringLiteral("Документ превышает лимит %1 МиБ").arg(maxFileSize_ / (1024 * 1024)));
        return;
    }
    pendingPath_ = info.canonicalFilePath();
    extractor_->extract(pendingPath_);
}

void StructuredDocumentReader::cancel() { extractor_->cancel(); }
