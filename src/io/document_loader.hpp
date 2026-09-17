#pragma once

#include "documents/document.hpp"

#include <QString>

class DocumentLoader {
    qint64 maxFileSize_ = 0;
public:
    explicit DocumentLoader(qint64 maxFileSize);

    Document loadTxtFile(const QString& path) const;
};

