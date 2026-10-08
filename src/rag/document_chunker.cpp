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

        if (!document.blocks.empty()) {
            int offset = 0;
            for (std::size_t index = 0; index < document.blocks.size();) {
                const auto& block = document.blocks[index];
                QString text = block.indexText();
                std::vector<std::pair<int, int>> starts{{0, static_cast<int>(index)}};
                ++index;
                if (text.trimmed().isEmpty()) continue;
                // Keep ordinary adjacent paragraphs together, as before structured import.
                // Images, tables, page changes and headers remain separate source boundaries.
                while (block.type == DocumentBlockType::Text && index < document.blocks.size()) {
                    const auto& next = document.blocks[index];
                    if (next.type != DocumentBlockType::Text || next.pageNumber != block.pageNumber ||
                        next.section != block.section || next.tableIndex != block.tableIndex ||
                        next.tableRow != block.tableRow || next.tableColumn != block.tableColumn) break;
                    const auto nextText = next.indexText();
                    if (!nextText.trimmed().isEmpty()) {
                        starts.push_back({text.size() + 1, static_cast<int>(index)});
                        text += "\n" + nextText;
                    }
                    ++index;
                }
                std::size_t source = 0;
                for (int start = 0; start < text.size(); start += chunkSize - overlap) {
                    while (source + 1 < starts.size() && starts[source + 1].first <= start) ++source;
                    Chunk chunk;
                    chunk.documentId = document.id;
                    chunk.sourcePath = document.sourcePath;
                    chunk.text = text.mid(start, chunkSize);
                    chunk.startOffset = offset + start;
                    chunk.blockIndex = starts[source].second;
                    chunk.pageNumber = block.pageNumber;
                    chunk.tableIndex = block.tableIndex;
                    chunk.tableRow = block.tableRow;
                    chunk.tableColumn = block.tableColumn;
                    chunk.imagePath = block.imagePath;
                    chunk.section = block.section;
                    result.push_back(chunk);
                    if (chunkSize >= text.size() - start) break;
                }
                offset += text.size() + 1;
            }
            return result;
        }

        int textSize = document.text.size();

        for (int startOffset = 0; startOffset < textSize; startOffset += chunkSize-overlap) {
            Chunk chunk;
            chunk.documentId = document.id;
            chunk.sourcePath = document.sourcePath;
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
