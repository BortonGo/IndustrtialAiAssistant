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
};
