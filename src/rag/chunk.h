#pragma once

#include <QString>

struct Chunk final {
    QString documentId;
    QString text;
    int startOffset;
};
