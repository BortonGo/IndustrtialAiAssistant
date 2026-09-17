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
    explicit DocumentManager(qint64 maxFileSize, QObject* parent = nullptr);
    bool loadTxtFile(const QString& path);
    const Document* findDocument(const QString& documentId) const;

    int documentCount() const;
    const Document* documentAt(int row) const;

signals:
    void errorOccurred(const QString &message);
    void documentLoaded(const QString &documentId);
    void documentAboutToBeAdded(int row);
};
