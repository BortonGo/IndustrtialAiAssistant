#pragma once

#include "document_block.hpp"

#include <QString>

#include <vector>

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
    std::vector<DocumentBlock> blocks;

    static QString statusToQString(const DocumentStatus& status_) {
        switch (status_) {
        case DocumentStatus::Pending :
            return "Ожидает";
        case DocumentStatus::Indexing :
            return "Индексация";
        case DocumentStatus::Ready :
            return "Готов";
        case DocumentStatus::Error :
        default:
            return "Ошибка";
        }
    }
};
