#pragma once

#include "documents/document.hpp"
#include "idocument_reader.hpp"

#include <QObject>
#include <QString>

class DocumentLoader : public QObject {
    Q_OBJECT

    qint64 maxFileSize_ = 0;
    IDocumentReader* txtReader_ = nullptr;
    IDocumentReader* pdfReader_ = nullptr;
public:
    explicit DocumentLoader(qint64 maxFileSize, const QString& pdfExtractorPath, QObject* parent = nullptr);

    void loadFile(const QString& path);

signals:
    void documentReady(const Document& document);
    void errorOccurred(const QString& message);
};

