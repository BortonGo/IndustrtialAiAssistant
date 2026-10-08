#include "vector_store.hpp"

#include <stdexcept>
#include <cmath>
#include <algorithm>
#include <utility>

namespace VectorStore {
    double cosineSimilarity(const std::vector<double> &a, const std::vector<double> &b) {
        if (a.empty() || b.empty() || a.size() != b.size()) {
            throw std::invalid_argument("vector size must be > 0 and equal");
        }

        double mul_sm = 0;
        double a_q_sm = 0;
        double b_q_sm = 0;
        std::size_t sz = a.size();
        for (std::size_t i = 0; i < sz; ++i) {
            mul_sm += a[i] * b[i];
            a_q_sm += a[i] * a[i];
            b_q_sm += b[i] * b[i];
        }
        double denominator = std::sqrt(a_q_sm) * std::sqrt(b_q_sm);
        if (a_q_sm == 0.0 || b_q_sm == 0.0) {
            throw std::invalid_argument("zero-length vector");
        }
        return mul_sm / denominator;
    }

    void InMemoryVectorStore::add(const Entry& entry) {
        if (entry.embedding.empty()) {
            throw std::invalid_argument("embedding vector is empty");
        }
        bool isZero = true;
        for (auto c : entry.embedding) {
            if (c != 0.0) {
                isZero = false;
            }
            if (!std::isfinite(c)) {
                throw std::invalid_argument("embedding have inf");
            }
        }
        if (isZero) {
            throw std::invalid_argument("embedding is zero vector");
        }
        if (!entries.empty() && entries.front().embedding.size() != entry.embedding.size()) {
            throw std::invalid_argument("different embedding size");
        }
        entries.push_back(entry);
    }

    std::vector<SearchResult> InMemoryVectorStore::search(const std::vector<double> &queryEmbedding, std::size_t topK,
                                                        const QStringList& documentIds) const {
        if (topK == 0 || entries.empty()) {
            return {};
        }
        bool isZero = true;
        for (auto c : queryEmbedding) {
            if (c != 0.0) {
                isZero = false;
            }
            if (!std::isfinite(c)) {
                throw std::invalid_argument("embedding have inf");
            }
        }
        if (isZero) {
            throw std::invalid_argument("embedding is zero vector");
        }
        if (!entries.empty() && entries.front().embedding.size() != queryEmbedding.size()) {
            throw std::invalid_argument("different embedding size");
        }
        std::vector<SearchResult> result;
        for (const auto& c : entries) {
            if (!documentIds.isEmpty() && !documentIds.contains(c.chunk.documentId)) continue;
            SearchResult sr {};
            sr.chunk = c.chunk;
            sr.score = cosineSimilarity(c.embedding, queryEmbedding);
            result.push_back(std::move(sr));
        }
        std::sort(result.begin(), result.end(), [](const SearchResult &a, const SearchResult &b) {return a.score > b.score;});
        if (result.size() > topK) {
            result.resize(topK);
        }
        return result;
    }

    void InMemoryVectorStore::removeDocument(const QString& documentId) {
        auto newEnd = std::remove_if(entries.begin(), entries.end(), [&documentId](const Entry& entry) {
            return entry.chunk.documentId == documentId;
        });
        entries.erase(newEnd, entries.end());
    }

    void InMemoryVectorStore::replaceDocument(const QString& documentId, std::vector<Entry> replacement) {
        if (documentId.isEmpty() || replacement.empty()) throw std::invalid_argument("Empty replacement index");
        const auto dimension = entries.empty() ? replacement.front().embedding.size() : entries.front().embedding.size();
        for (const auto& entry : replacement) {
            if (entry.chunk.documentId != documentId || entry.embedding.empty() || entry.embedding.size() != dimension)
                throw std::invalid_argument("Invalid replacement document or embedding dimension");
            bool nonzero = false;
            for (const auto value : entry.embedding) {
                if (!std::isfinite(value)) throw std::invalid_argument("Non-finite replacement embedding");
                nonzero = nonzero || value != 0.0;
            }
            if (!nonzero) throw std::invalid_argument("Zero replacement embedding");
        }
        const auto retained = std::count_if(entries.begin(), entries.end(), [&](const Entry& entry) {
            return entry.chunk.documentId != documentId;
        });
        // Validate and allocate before touching the searchable version. The incoming
        // entries already own their vectors; moving them below needs no allocations.
        entries.reserve(static_cast<std::size_t>(retained) + replacement.size());
        removeDocument(documentId);
        for (auto& entry : replacement) entries.push_back(std::move(entry));
    }

    void InMemoryVectorStore::clear() {
        entries.clear();
    }

    std::vector<Entry> InMemoryVectorStore::documentEntries(const QString& documentId) const {
        std::vector<Entry> result;
        for (const auto& entry : entries) if (entry.chunk.documentId == documentId) result.push_back(entry);
        return result;
    }

    bool InMemoryVectorStore::empty() const {
        return entries.empty();
    }

}
