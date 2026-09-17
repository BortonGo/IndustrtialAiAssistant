#pragma once

#include <QString>

enum class DocumentStatus {
    Pending,
    Indexing,
    Ready,
    Error
};

struct Document final {
    QString id;
    QString sourcePath;
    QString text;
    DocumentStatus status = DocumentStatus::Pending;
};
