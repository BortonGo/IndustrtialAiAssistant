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
