#pragma once

#include <QString>

struct Chunk final {
    QString documentId;
    QString sourcePath;
    QString text;
    int startOffset = 0;
    int pageNumber = 0;
    int blockIndex = -1;
    int tableIndex = -1;
    int tableRow = -1;
    int tableColumn = -1;
    QString imagePath;
    QString section;

    QString sourceDescription() const {
        QString result = "Источник: " + (sourcePath.isEmpty() ? documentId : sourcePath);
        if (pageNumber > 0) result += QStringLiteral("\nСтраница: %1").arg(pageNumber);
        if (!section.isEmpty()) result += "\n" + section;
        if (tableIndex >= 0) result += QStringLiteral("\nТаблица: %1").arg(tableIndex + 1);
        if (tableRow >= 0) result += QStringLiteral(", строка: %1, столбец: %2").arg(tableRow + 1).arg(tableColumn + 1);
        if (!imagePath.isEmpty()) result += QStringLiteral("\nИзображение (описание моделью; возможны ошибки)");
        result += QStringLiteral("\nПозиция в извлечённом тексте: %1").arg(startOffset);
        return result;
    }
};
