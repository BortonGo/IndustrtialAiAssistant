#pragma once

#include "chunk.h"
#include <QStringList>
#include <QHash>
#include <QSet>

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
        struct IndexedEntry {
            Entry value;
            QHash<QString, int> terms;
            QSet<QString> tokens;
            QString normalizedText;
            int termCount = 0;
        };
        std::vector<IndexedEntry> entries;
        static IndexedEntry indexEntry(Entry entry);
    public:
        void add(const Entry& entry);

        std::vector<SearchResult> search(const std::vector<double> &queryEmbedding, std::size_t topK,
                                         const QStringList& documentIds = {}) const;

        // RRF/BM25 score is a ranking value, not a cosine similarity or probability.
        std::vector<SearchResult> searchHybrid(const QString& query, const std::vector<double>& queryEmbedding,
                                               std::size_t topK, const QStringList& documentIds = {}) const;

        void removeDocument(const QString& documentId);
        void replaceDocument(const QString& documentId, std::vector<Entry> replacement);

        void clear();
        std::vector<Entry> documentEntries(const QString& documentId) const;

        bool empty() const;
    };
}
