#pragma once

#include "document_block.hpp"

#include <QString>

#include <vector>

enum class DocumentStatus {
    Pending,
    Indexing,
    Ready,
    ReadyWithWarnings,
    TextReady,
    Error
};

struct Document final {
    QString id;
    QString sourcePath;
    QString text;
    DocumentStatus status = DocumentStatus::Pending;
    std::vector<DocumentBlock> blocks;
    QStringList warnings;
    int pendingImages = 0;

    static QString statusToQString(const DocumentStatus& status_) {
        switch (status_) {
        case DocumentStatus::Pending :
            return "Ожидает";
        case DocumentStatus::Indexing :
            return "Индексация";
        case DocumentStatus::Ready :
            return "Готов";
        case DocumentStatus::ReadyWithWarnings :
            return "Готов с предупреждениями";
        case DocumentStatus::TextReady :
            return "Текст готов · изображения не распознаны";
        case DocumentStatus::Error :
        default:
            return "Ошибка";
        }
    }
};
