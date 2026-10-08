#include "document_manager.hpp"

#include <stdexcept>
#include <QFileInfo>
#include <QUuid>

DocumentManager::DocumentManager(const DocumentExtractionConfig& config, QObject* parent)
    : QObject(parent), documentLoader_(config) {
    documents_.reserve(20);

    connect(&documentLoader_, &DocumentLoader::documentReady,
            this, [this](const Document& content) {
        Document document = content;
        document.id = QUuid::createUuid().toString().mid(1, 36);
        for (const auto& existing : documents_) {
            if (QString::compare(existing.sourcePath, document.sourcePath, Qt::CaseInsensitive) == 0) {
                document.id = existing.id;
                break;
            }
        }
        if (auto* previous = find(document.id)) {
            *previous = document;
            emit documentStatusChanged(document.id);
            emit documentIndexingRequested(document.id);
            return;
        }
        emit documentAboutToBeAdded(documentCount());
        documents_.push_back(document);
        emit documentLoaded(documents_.back().id);
        emit documentIndexingRequested(documents_.back().id);
        return;
    });

    connect(&documentLoader_, &DocumentLoader::errorOccurred,
            this, &DocumentManager::errorOccurred);
    connect(&documentLoader_, &DocumentLoader::progressChanged,
            this, &DocumentManager::progressChanged);
}

void DocumentManager::loadFile(const QString& path) {
    const Document* existing = nullptr;
    const auto source = QFileInfo(path).canonicalFilePath();
    for (const auto& document : documents_) {
        if (QString::compare(document.sourcePath, source, Qt::CaseInsensitive) == 0) { existing = &document; break; }
    }
    if (existing && existing->status != DocumentStatus::Error && existing->status != DocumentStatus::ReadyWithWarnings) {
        emit errorOccurred(QStringLiteral("Документ уже загружен"));
        return;
    }
    documentLoader_.loadFile(path);
}

void DocumentManager::cancelLoading() { documentLoader_.cancel(); }

void DocumentManager::restore(std::vector<Document> documents) {
    emit documentsAboutToReset();
    documents_ = std::move(documents);
    emit documentsReset();
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
