#include "document_manager.hpp"

#include <stdexcept>
#include <utility>

DocumentManager::DocumentManager(qint64 maxFileSize, QObject* parent)
    : QObject(parent), documentLoader_(maxFileSize) {
    documents_.reserve(20);
}

bool DocumentManager::loadTxtFile(const QString& path) {
    Document d;
    try {
        d = documentLoader_.loadTxtFile(path);
    } catch(const std::runtime_error& e) {
        emit errorOccurred(QString::fromUtf8(e.what()));
        return false;
    }
    if (findDocument(d.id)) {
        emit errorOccurred("Document is already loaded");
        return false;
    }
    emit documentAboutToBeAdded(documentCount());
    documents_.push_back(std::move(d));
    emit documentLoaded(documents_.back().id);
    return true;
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
