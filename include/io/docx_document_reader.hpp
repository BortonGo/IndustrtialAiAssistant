#pragma once

#include "documents/document.hpp"
#include "idocument_reader.hpp"

#include <QObject>
#include <QString>
class DocxTextExtractor;

class DocxDocumentReader final : public IDocumentReader {
    Q_OBJECT

    DocxTextExtractor* extractor_ = nullptr;
    QString pendingPath_;
    qint64 maxFileSize_ = 0;
    bool busy_ = false;

public:
    explicit DocxDocumentReader(const QString& pythonPath,
                                const QString& scriptPath,
                               qint64 maxFileSize,
                               QObject* parent = nullptr);
    void load(const QString& path) override;
};
