#include "document_loader.hpp"
#include "txt_document_reader.hpp"
#include "pdf_document_reader.hpp"

#include <QFile>
#include <QFileInfo>
#include <QByteArray>

#include <stdexcept>
#include <utility>

DocumentLoader::DocumentLoader(qint64 maxFileSize, const QString& pdfExtractorPath, QObject* parent) :
    QObject(parent), maxFileSize_(maxFileSize) {
    if (maxFileSize <= 0) {
        throw std::invalid_argument("Maximum document size must be > 0");
    }
    txtReader_ = new TxtDocumentReader(maxFileSize, this);

    connect(txtReader_, &IDocumentReader::documentReady,
            this, &DocumentLoader::documentReady);

    connect(txtReader_, &IDocumentReader::errorOccurred,
            this, &DocumentLoader::errorOccurred);

    pdfReader_ = new PdfDocumentReader(pdfExtractorPath, maxFileSize, this);

    connect(pdfReader_, &IDocumentReader::documentReady,
            this, &DocumentLoader::documentReady);

    connect(pdfReader_, &IDocumentReader::errorOccurred,
            this, &DocumentLoader::errorOccurred);
}

void DocumentLoader::loadFile(const QString& path) {
    auto suffix = QFileInfo(path).suffix().toLower();
    if (suffix == "txt") {
        txtReader_->load(path);
    } else if (suffix == "pdf") {
        pdfReader_->load(path);
    } else {
        emit errorOccurred("Unexpected file format");
    }
}

