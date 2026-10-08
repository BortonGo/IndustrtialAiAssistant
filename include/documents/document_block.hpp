#pragma once

#include <QString>
#include <QStringList>

#include <vector>

enum class DocumentBlockType {
    Text,
    Table,
    Image
};

struct DocumentBlock {
    DocumentBlockType type = DocumentBlockType::Text;
    QString text;
    std::vector<QStringList> tableRows;
    QString imagePath;
    int pageNumber = 0;
    int tableIndex = -1;
    int tableRow = -1;
    int tableColumn = -1;
    QString section;

    QString indexText() const {
        if (type == DocumentBlockType::Table) {
            QStringList rows;
            for (const auto& row : tableRows) {
                const QString value = row.join("\t");
                if (!value.trimmed().isEmpty()) rows.append(value);
            }
            return rows.join("\n");
        }
        if (type == DocumentBlockType::Image && !text.trimmed().isEmpty())
            return QStringLiteral("[Описание изображения моделью; возможны ошибки]\n") + text;
        return text;
    }
};
