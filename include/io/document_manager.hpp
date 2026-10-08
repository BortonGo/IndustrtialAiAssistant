#pragma once

#include "document_loader.hpp"
#include "documents/document.hpp"

#include <QObject>
#include <QString>

#include <vector>

class DocumentManager final : public QObject {
    Q_OBJECT

    DocumentLoader documentLoader_;
    std::vector<Document> documents_;
public:
    explicit DocumentManager(const DocumentExtractionConfig& config, QObject* parent = nullptr);
    void loadFile(const QString& path);
    void cancelLoading();
    const Document* findDocument(const QString& documentId) const;

    int documentCount() const;
    const Document* documentAt(int row) const;

    bool setDocumentStatus(const QString& documentId, const DocumentStatus& status);
    void restore(std::vector<Document> documents);

signals:
    void documentsAboutToReset();
    void documentsReset();
    void errorOccurred(const QString &message);
    void documentLoaded(const QString &documentId);
    void documentIndexingRequested(const QString& documentId);
    void documentAboutToBeAdded(int row);
    void documentStatusChanged(const QString& documentId);
    void progressChanged(const QString& message);

private:
    Document* find(const QString& documentId);
};
