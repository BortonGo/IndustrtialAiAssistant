#pragma once
#include "idocument_reader.hpp"
#include "document_extractor.hpp"

class StructuredDocumentReader final : public IDocumentReader {
    Q_OBJECT
public:
    explicit StructuredDocumentReader(const DocumentExtractionConfig& config, QObject* parent = nullptr);
    void load(const QString& path) override;
    void cancel() override;
private:
    DocumentExtractor* extractor_;
    qint64 maxFileSize_;
    QString pendingPath_;
};
