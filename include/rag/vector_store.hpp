#pragma once

#include "chunk.h"

#include <vector>

namespace VectorStore {
    double cosineSimilarity(const std::vector<double> &a, const std::vector<double> &b);

    struct Entry {
        Chunk chunk;
        std::vector<double> embedding;
    };

    struct SearchResult {
        Chunk chunk;
        double score;
    };

    class InMemoryVectorStore final {
        std::vector<Entry> entries;
    public:
        void add(const Entry& entry);

        std::vector<SearchResult> search(const std::vector<double> &queryEmbedding, std::size_t topK) const;

        void removeDocument(const QString& documentId);

        void clear();

        bool empty() const;
    };
}
