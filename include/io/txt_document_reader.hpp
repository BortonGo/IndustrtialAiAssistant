#pragma once

#include "documents/document.hpp"
#include "idocument_reader.hpp"

#include <QObject>
#include <QString>

class TxtDocumentReader final : public IDocumentReader {
    Q_OBJECT

    qint64 maxFileSize_ = 0;

    Document loadFile(const QString& path) const;
public:
    explicit TxtDocumentReader(qint64 maxFileSize, QObject* parent = nullptr);
    void load(const QString& path) override;
};
