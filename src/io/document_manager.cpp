#include "document_manager.hpp"

#include <stdexcept>

DocumentManager::DocumentManager(qint64 maxFileSize, const QString& pdfExtractorPath, QObject* parent)
    : QObject(parent), documentLoader_(maxFileSize, pdfExtractorPath) {
    documents_.reserve(20);

    connect(&documentLoader_, &DocumentLoader::documentReady,
            this, [this](const Document& document) {
        if (findDocument(document.id)) {
            emit errorOccurred("Document is already loaded");
            return;
        }
        emit documentAboutToBeAdded(documentCount());
        documents_.push_back(document);
        emit documentLoaded(documents_.back().id);
        return;
    });

    connect(&documentLoader_, &DocumentLoader::errorOccurred,
            this, &DocumentManager::errorOccurred);
}

void DocumentManager::loadFile(const QString& path) {
    documentLoader_.loadFile(path);
}

const Document* DocumentManager::findDocument(const QString& documentId) const {
    for (const auto& d : documents_) {
        if (d.id == documentId) {
            return &d;
        }
    }
    return nullptr;
}

int DocumentManager::documentCount() const {
    return static_cast<int>(documents_.size());
}

const Document* DocumentManager::documentAt(int row) const {
    if (row < 0 || row >= documentCount()) {
        return nullptr;
    }
    return &documents_[row];
}

bool DocumentManager::setDocumentStatus(const QString& documentId, const DocumentStatus& status) {
    auto* d = find(documentId);
    if (!d) {
        return false;
    }
    if (d->status == status) {
        return true;
    }
    d->status = status;
    emit documentStatusChanged(documentId);
    return true;
}

Document* DocumentManager::find(const QString& documentId) {
    for (auto& d : documents_) {
        if (d.id == documentId) {
            return &d;
        }
    }
    return nullptr;
}
