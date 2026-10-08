#include "document_loader.hpp"
#include "txt_document_reader.hpp"
#include "structured_document_reader.hpp"

#include <QFile>
#include <QFileInfo>
#include <QByteArray>

#include <stdexcept>
#include <utility>

DocumentLoader::DocumentLoader(const DocumentExtractionConfig& config, QObject* parent) :
    QObject(parent), maxFileSize_(config.maxFileSize) {
    if (maxFileSize_ <= 0) {
        throw std::invalid_argument("Maximum document size must be > 0");
    }
    txtReader_ = new TxtDocumentReader(maxFileSize_, this);

    connect(txtReader_, &IDocumentReader::documentReady,
            this, &DocumentLoader::documentReady);

    connect(txtReader_, &IDocumentReader::errorOccurred,
            this, &DocumentLoader::errorOccurred);

    structuredReader_ = new StructuredDocumentReader(config, this);
    connect(structuredReader_, &IDocumentReader::documentReady,
            this, &DocumentLoader::documentReady);
    connect(structuredReader_, &IDocumentReader::errorOccurred,
            this, &DocumentLoader::errorOccurred);
    connect(structuredReader_, &IDocumentReader::progressChanged,
            this, &DocumentLoader::progressChanged);
}

void DocumentLoader::loadFile(const QString& path) {
    auto suffix = QFileInfo(path).suffix().toLower();
    if (suffix == "txt") {
        txtReader_->load(path);
    } else if (suffix == "pdf" || suffix == "docx" || suffix == "doc" ||
               suffix == "png" || suffix == "jpg" || suffix == "jpeg" ||
               suffix == "webp" || suffix == "bmp") {
        structuredReader_->load(path);
    } else {
        emit errorOccurred(QStringLiteral("Поддерживаются TXT, PDF, DOC, DOCX и изображения PNG, JPG, JPEG, WEBP, BMP"));
    }
}

void DocumentLoader::cancel() { structuredReader_->cancel(); }

