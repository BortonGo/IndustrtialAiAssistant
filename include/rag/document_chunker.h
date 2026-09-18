#pragma once

#include "chunk.h"
#include "documents/document.hpp"

#include <vector>

namespace DocumentChunker {
    std::vector<Chunk> chunkDocument(const Document& document, int chunkSize, int overlap);
}
