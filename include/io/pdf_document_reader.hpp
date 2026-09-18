#pragma once

#include "documents/document.hpp"
#include "idocument_reader.hpp"
#include "pdf_text_extractor.hpp"

#include <QObject>
#include <QString>

class PdfDocumentReader final : public IDocumentReader {
    Q_OBJECT

    PdfTextExtractor* extractor_ = nullptr;
    QString pendingPath_;
    qint64 maxFileSize_ = 0;
    bool busy_ = false;
public:
    explicit PdfDocumentReader(const QString& executablePath,
                               qint64 maxFileSize,
                               QObject* parent = nullptr);
    void load(const QString& path) override;
};
