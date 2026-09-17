#include "document_chunker.h"

#include <stdexcept>

namespace DocumentChunker {
    std::vector<Chunk> chunkDocument(const Document& document, int chunkSize, int overlap) {
        if (chunkSize <= 0) {
            throw std::invalid_argument("chunkSize must be > 0");
        }
        if (overlap < 0 || overlap >= chunkSize) {
            throw std::invalid_argument("must be 0 <= overlap < chunkSize");
        }
        std::vector<Chunk> result;
        result.reserve(document.text.size() / chunkSize);

        int textSize = document.text.size();

        for (int startOffset = 0; startOffset < textSize; startOffset += chunkSize-overlap) {
            Chunk chunk;
            chunk.documentId = document.id;
            chunk.startOffset = startOffset;
            chunk.text = document.text.mid(startOffset, chunkSize);

            result.push_back(chunk);

            if (chunkSize >= textSize - startOffset) {
                break;
            }
        }
        return result;
    }
}
