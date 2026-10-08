#pragma once

#include "documents/document.hpp"
#include "idocument_reader.hpp"
#include "document_extractor.hpp"

#include <QObject>
#include <QString>

class DocumentLoader : public QObject {
    Q_OBJECT

    qint64 maxFileSize_ = 0;
    IDocumentReader* txtReader_ = nullptr;
    IDocumentReader* structuredReader_ = nullptr;
public:
    explicit DocumentLoader(const DocumentExtractionConfig& config, QObject* parent = nullptr);

    void loadFile(const QString& path);
    void cancel();

signals:
    void documentReady(const Document& document);
    void errorOccurred(const QString& message);
    void progressChanged(const QString& message);
};

