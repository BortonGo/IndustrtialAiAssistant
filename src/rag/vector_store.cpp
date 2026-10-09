#include "vector_store.hpp"

#include <stdexcept>
#include <cmath>
#include <algorithm>
#include <utility>
#include <QRegularExpression>

namespace {
QString normalized(QString text, bool fold = true) {
    text = text.normalized(QString::NormalizationForm_KC);
    for (auto& c : text) {
        if ((c.unicode() >= 0x2010 && c.unicode() <= 0x2015) || c.unicode() == 0x2212) c = '-';
        if (c == QChar(0x451)) c = QChar(0x435);
        if (c == QChar(0x401)) c = QChar(0x415);
    }
    text.remove(QChar(0xad)); // Soft hyphens introduced by PDF layout.
    static const QRegularExpression spacedDash("\\s*([-_])\\s*");
    static const QRegularExpression spacedNumber("([0-9])\\s*\\.\\s*(?=[0-9])");
    text.replace(spacedDash, "\\1"); text.replace(spacedNumber, "\\1.");
    return (fold ? text.toCaseFolded() : text).simplified();
}
const QRegularExpression& tokenPattern() {
    static const QRegularExpression pattern(QStringLiteral("[\\p{L}\\p{N}]+(?:[-_./][\\p{L}\\p{N}]+)*"),
        QRegularExpression::UseUnicodePropertiesOption);
    return pattern;
}
bool stopWord(const QString& word) {
    static const QSet<QString> words = []() {
        QSet<QString> result;
        for (const auto& value : QStringLiteral("а без бы был была были было в во вы да для до его ее если есть же за и из или к как какой какая какие какое ко ли мне мы на не но о об он она они от по при про с со так то у что это я the a an and or of to in on for is are was were be with how what which please").split(' '))
            result.insert(value);
        return result;
    }();
    return words.contains(word);
}
struct Anchor { QString text; bool phrase = false; };
std::vector<Anchor> anchors(const QString& query) {
    std::vector<Anchor> result;
    QSet<QString> added;
    auto append = [&](const QString& value, bool phrase) {
        const auto text = normalized(value);
        if (!text.isEmpty() && !added.contains(text)) { result.push_back({text, phrase}); added.insert(text); }
    };
    auto tokens = tokenPattern().globalMatch(normalized(query, false));
    while (tokens.hasNext()) {
        const auto token = tokens.next().captured();
        bool letter = false, digit = false;
        for (const auto c : token) { letter = letter || c.isLetter(); digit = digit || c.isDigit(); }
        // Preserve complete identifiers (including leading zeros), not substrings.
        if ((digit && (letter || token.size() >= 2)) || (letter && token.contains('_')) ||
            (letter && token.size() >= 2 && token.size() <= 12 && token == token.toUpper() && !stopWord(token.toCaseFolded())))
            append(token, false);
    }
    static const QRegularExpression quoted(QStringLiteral("[\"«“]([^\"»”\\n]{2,120})[\"»”]"));
    auto phrases = quoted.globalMatch(query);
    while (phrases.hasNext()) {
        const auto phrase = normalized(phrases.next().captured(1));
        const auto match = tokenPattern().match(phrase);
        append(phrase, match.capturedLength() != phrase.size());
    }
    return result;
}
bool phraseMatches(const QString& text, const QString& phrase) {
    auto word = [](QChar c) { return c.isLetterOrNumber() || c == '_' || c == '-'; };
    int from = 0;
    while ((from = text.indexOf(phrase, from)) >= 0) {
        const int end = from + phrase.size();
        if ((from == 0 || !word(text[from-1])) && (end == text.size() || !word(text[end]))) return true;
        ++from;
    }
    return false;
}
void validateQuery(const std::vector<double>& query, std::size_t dimension) {
    if (query.size() != dimension) throw std::invalid_argument("different embedding size");
    bool nonzero = false;
    for (const auto x : query) {
        if (!std::isfinite(x)) throw std::invalid_argument("embedding have inf");
        nonzero = nonzero || x != 0;
    }
    if (!nonzero) throw std::invalid_argument("embedding is zero vector");
}
}

namespace VectorStore {
    InMemoryVectorStore::IndexedEntry InMemoryVectorStore::indexEntry(Entry entry) {
        IndexedEntry indexed; indexed.value = std::move(entry);
        indexed.normalizedText = normalized(indexed.value.chunk.text);
        auto tokens = tokenPattern().globalMatch(indexed.normalizedText);
        auto add = [&](const QString& token) {
            if (!stopWord(token)) { ++indexed.terms[token]; ++indexed.termCount; }
        };
        static const QRegularExpression separator("[-_./]");
        while (tokens.hasNext()) {
            const auto token = tokens.next().captured();
            indexed.tokens.insert(token); add(token);
            const auto parts = token.split(separator, QString::SkipEmptyParts);
            if (parts.size() > 1) for (const auto& part : parts) add(part);
        }
        return indexed;
    }
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
        if (!entries.empty() && entries.front().value.embedding.size() != entry.embedding.size()) {
            throw std::invalid_argument("different embedding size");
        }
        entries.push_back(indexEntry(entry));
    }

    std::vector<SearchResult> InMemoryVectorStore::search(const std::vector<double> &queryEmbedding, std::size_t topK,
                                                        const QStringList& documentIds) const {
        if (topK == 0 || entries.empty()) {
            return {};
        }
        validateQuery(queryEmbedding, entries.front().value.embedding.size());
        std::vector<SearchResult> result;
        for (const auto& c : entries) {
            if (!documentIds.isEmpty() && !documentIds.contains(c.value.chunk.documentId)) continue;
            SearchResult sr {};
            sr.chunk = c.value.chunk;
            sr.score = cosineSimilarity(c.value.embedding, queryEmbedding);
            result.push_back(std::move(sr));
        }
        std::stable_sort(result.begin(), result.end(), [](const SearchResult &a, const SearchResult &b) {return a.score > b.score;});
        if (result.size() > topK) {
            result.resize(topK);
        }
        return result;
    }

    std::vector<SearchResult> InMemoryVectorStore::searchHybrid(const QString& query, const std::vector<double>& vector,
                                                               std::size_t topK, const QStringList& documentIds) const {
        if (entries.empty() || topK == 0) return {};
        validateQuery(vector, entries.front().value.embedding.size());
        Entry queryEntry; queryEntry.chunk.text = query;
        auto queryTerms = indexEntry(queryEntry).terms.keys(); queryTerms.sort();
        const auto exact = anchors(query);
        if (queryTerms.isEmpty() && exact.empty()) return search(vector, topK, documentIds);
        struct Candidate {
            std::size_t index;
            double semantic = 0, lexical = 0, fused = 0;
            QSet<int> anchors;
        };
        std::vector<Candidate> candidates;
        QHash<QString, int> frequencies;
        double totalLength = 0;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto& entry = entries[i];
            if (!documentIds.isEmpty() && !documentIds.contains(entry.value.chunk.documentId)) continue;
            candidates.push_back({i, cosineSimilarity(entry.value.embedding, vector), 0, 0, {}});
            totalLength += entry.termCount;
            for (const auto& term : queryTerms) if (entry.terms.contains(term)) ++frequencies[term];
        }
        if (candidates.empty()) return {};
        const double averageLength = std::max(1.0, totalLength / candidates.size());
        std::vector<std::size_t> semantic, lexical;
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            auto& candidate = candidates[i]; const auto& entry = entries[candidate.index];
            for (const auto& term : queryTerms) {
                const double count = entry.terms.value(term);
                if (!count) continue;
                const double df = frequencies.value(term);
                const double idf = std::log(1.0 + (candidates.size() - df + 0.5) / (df + 0.5));
                candidate.lexical += idf * (count * 2.2) / (count + 1.2 * (0.25 + 0.75 * entry.termCount / averageLength));
            }
            for (std::size_t j = 0; j < exact.size(); ++j)
                if (exact[j].phrase ? phraseMatches(entry.normalizedText, exact[j].text) : entry.tokens.contains(exact[j].text))
                    candidate.anchors.insert(static_cast<int>(j));
            semantic.push_back(i);
            if (candidate.lexical > 0) lexical.push_back(i);
        }
        if (lexical.empty() && std::none_of(candidates.begin(), candidates.end(), [](const Candidate& c) { return !c.anchors.isEmpty(); }))
            return search(vector, topK, documentIds);
        auto rank = [&](std::vector<std::size_t>& order, bool words) {
            std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
                return words ? candidates[a].lexical > candidates[b].lexical : candidates[a].semantic > candidates[b].semantic;
            });
        };
        rank(semantic, false); rank(lexical, true);
        const auto window = std::max<std::size_t>(64, topK);
        // Reciprocal rank fusion avoids comparing BM25's unbounded score with cosine.
        for (const auto* list : {&semantic, &lexical})
            for (std::size_t r = 0; r < std::min(window, list->size()); ++r)
                candidates[(*list)[r]].fused += 1.0 / (60.0 + r + 1);
        for (auto& candidate : candidates)
            candidate.fused += candidate.anchors.size() * (2.0 / 61.0);
        std::stable_sort(semantic.begin(), semantic.end(), [&](std::size_t a, std::size_t b) {
            if (candidates[a].fused != candidates[b].fused) return candidates[a].fused > candidates[b].fused;
            if (candidates[a].semantic != candidates[b].semantic) return candidates[a].semantic > candidates[b].semantic;
            return candidates[a].index < candidates[b].index;
        });
        std::vector<std::size_t> chosen;
        QSet<int> covered;
        // A comparison of two codes must not spend every slot on the first one.
        for (const auto i : semantic) {
            auto missing = candidates[i].anchors; missing.subtract(covered);
            if (missing.isEmpty()) continue;
            chosen.push_back(i); covered.unite(candidates[i].anchors);
            if (chosen.size() == topK) break;
        }
        for (const auto i : semantic) {
            if (chosen.size() == topK) break;
            if (std::find(chosen.begin(), chosen.end(), i) == chosen.end()) chosen.push_back(i);
        }
        // Keep coverage representatives first: downstream context budgets may use
        // fewer hits than topK, and must still see both codes in a comparison.
        std::vector<SearchResult> result;
        for (const auto i : chosen) result.push_back({entries[candidates[i].index].value.chunk, candidates[i].fused});
        return result;
    }

    void InMemoryVectorStore::removeDocument(const QString& documentId) {
        auto newEnd = std::remove_if(entries.begin(), entries.end(), [&documentId](const IndexedEntry& entry) {
            return entry.value.chunk.documentId == documentId;
        });
        entries.erase(newEnd, entries.end());
    }

    void InMemoryVectorStore::replaceDocument(const QString& documentId, std::vector<Entry> replacement) {
        if (documentId.isEmpty() || replacement.empty()) throw std::invalid_argument("Empty replacement index");
        const auto dimension = entries.empty() ? replacement.front().embedding.size() : entries.front().value.embedding.size();
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
        std::vector<IndexedEntry> indexed; indexed.reserve(replacement.size());
        for (auto& entry : replacement) indexed.push_back(indexEntry(std::move(entry)));
        const auto retained = std::count_if(entries.begin(), entries.end(), [&](const IndexedEntry& entry) {
            return entry.value.chunk.documentId != documentId;
        });
        // Validate and allocate before touching the searchable version. The incoming
        // entries already own their vectors; moving them below needs no allocations.
        entries.reserve(static_cast<std::size_t>(retained) + replacement.size());
        removeDocument(documentId);
        for (auto& entry : indexed) entries.push_back(std::move(entry));
    }

    void InMemoryVectorStore::clear() {
        entries.clear();
    }

    std::vector<Entry> InMemoryVectorStore::documentEntries(const QString& documentId) const {
        std::vector<Entry> result;
        for (const auto& entry : entries) if (entry.value.chunk.documentId == documentId) result.push_back(entry.value);
        return result;
    }

    bool InMemoryVectorStore::empty() const {
        return entries.empty();
    }

}
