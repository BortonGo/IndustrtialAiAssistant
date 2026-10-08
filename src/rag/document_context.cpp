#include "rag/document_context.hpp"
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

namespace {
QString normalized(QString value) {
    value = value.toCaseFolded(); value.replace(QChar(0x451), QChar(0x435));
    value.replace(QRegularExpression(QStringLiteral("[^\\p{L}\\p{N}]+"),
                                    QRegularExpression::UseUnicodePropertiesOption), " ");
    return value.simplified();
}
bool ready(const Document* document) {
    return document && (document->status == DocumentStatus::Ready || document->status == DocumentStatus::ReadyWithWarnings);
}
bool heading(const DocumentBlock& block) {
    if (block.type != DocumentBlockType::Text) return false;
    static const QRegularExpression pattern(QStringLiteral("^(?:\\d+(?:\\.\\d+)+[.)]?\\s+|таблица\\s+\\d|раздел\\s+\\d)"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
    return pattern.match(block.text.trimmed()).hasMatch();
}
QString blockText(const DocumentBlock& block) {
    if (block.type != DocumentBlockType::Table) return block.indexText();
    // Keep column boundaries and empty cells. Never split a row to meet a character limit.
    QStringList lines;
    int columns = 0;
    for (const auto& row : block.tableRows) columns = std::max(columns, row.size());
    for (std::size_t i = 0; i < block.tableRows.size(); ++i) {
        auto cells = block.tableRows[i];
        while (cells.size() < columns) cells.append(QString());
        for (auto& cell : cells) { cell.replace('|', "\\|"); cell.replace('\n', "<br>"); }
        lines.append("| " + cells.join(" | ") + " |");
        if (i == 0) {
            QStringList rule; for (int c = 0; c < columns; ++c) rule.append("---");
            lines.append("| " + rule.join(" | ") + " |");
        }
    }
    return QStringLiteral("[Таблица: %1 строк; пустые ячейки сохранены]\n").arg(block.tableRows.size()) + lines.join('\n');
}
struct Range { int first; int last; }; // Exclusive end.
Range tableRange(const Document& document, int index) {
    int first = index, last = index + 1;
    // Captions and section titles precede tables in DOCX as independent paragraphs.
    for (int i = index - 1; i >= 0 && index - i <= 4; --i) {
        const auto& block = document.blocks[i];
        if (block.type != DocumentBlockType::Text) break;
        first = i;
        if (heading(block) && !block.text.trimmed().startsWith(QStringLiteral("Таблица"), Qt::CaseInsensitive)) break;
    }
    // Include the explanatory prose until the next titled section/table, not just its first line.
    while (last < static_cast<int>(document.blocks.size())) {
        const auto& block = document.blocks[last];
        if (block.type == DocumentBlockType::Table || heading(block)) break;
        ++last;
    }
    return {first, last};
}
int nearbyTable(const Document& document, int blockIndex) {
    if (blockIndex < 0 || blockIndex >= static_cast<int>(document.blocks.size())) return -1;
    // An image hit must keep its pixels, even if a table happens to follow it.
    if (document.blocks[blockIndex].type == DocumentBlockType::Image) return -1;
    if (document.blocks[blockIndex].type == DocumentBlockType::Table) return blockIndex;
    for (int i = blockIndex + 1; i < static_cast<int>(document.blocks.size()) && i <= blockIndex + 4; ++i) {
        if (document.blocks[i].type == DocumentBlockType::Table) return i;
        if (document.blocks[i].type == DocumentBlockType::Image) break;
    }
    for (int i = blockIndex - 1; i >= 0; --i) {
        if (heading(document.blocks[i])) break;
        if (document.blocks[i].type == DocumentBlockType::Table) return i;
    }
    return -1;
}
QStringList tableReferences(const QString& text) {
    static const QRegularExpression pattern(QStringLiteral(
        "(?:таблиц\\p{L}*|tables?)\\s*№?\\s*(\\d+(?:\\.\\d+)*(?:\\s*(?:,|и|and|&)\\s*№?\\s*\\d+(?:\\.\\d+)*)*)"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
    static const QRegularExpression number("\\d+(?:\\.\\d+)*");
    QStringList references;
    auto mentions = pattern.globalMatch(text);
    while (mentions.hasNext()) {
        auto ids = number.globalMatch(mentions.next().captured(1));
        while (ids.hasNext()) {
            const auto id = ids.next().captured();
            if (!references.contains(id)) references.append(id);
        }
    }
    return references;
}
bool exhaustive(const QString& question) {
    static const QRegularExpression pattern(QStringLiteral("\\b(?:все|всё|весь|всех|целиком|полностью|кажд\\p{L}*|all|every|entire)\\b"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
    return pattern.match(question).hasMatch();
}
bool explicitlyWholeDocument(const QString& question) {
    static const QRegularExpression pattern(QStringLiteral(
        "\\b(?:(?:весь|всю|все|всё)\\s+(?:документ|файл|книг)\\p{L}*|"
        "(?:документ|файл|книг)\\p{L}*\\s+(?:целиком|полностью)|"
        "(?:целиком|полностью)\\s+(?:весь\\s+|всю\\s+)?(?:документ|файл|книг)\\p{L}*|"
        "(?:whole|entire)\\s+(?:document|file|book)|all\\s+of\\s+(?:the\\s+)?(?:document|file|book))\\b"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
    return pattern.match(question).hasMatch();
}
}

QStringList DocumentContext::namedDocuments(const QString& question, const std::vector<const Document*>& documents) {
    QStringList found;
    const auto query = normalized(question);
    const auto queryWords = query.split(' ', QString::SkipEmptyParts);
    for (const auto* document : documents) {
        if (!ready(document)) continue;
        const auto file = QFileInfo(document->sourcePath);
        const auto stem = normalized(file.completeBaseName());
        if (stem.size() < 4) continue;
        bool matches = (" " + query + " ").contains(" " + stem + " ");
        if (!matches) {
            // Tolerate punctuation/spacing and inflection of long Russian words in a filename.
            const auto words = stem.split(' ', QString::SkipEmptyParts);
            int significant = 0, matched = 0;
            bool conflictingNumber = false;
            for (const auto& word : words) {
                if (word.size() == 1 && !word[0].isDigit()) continue;
                ++significant;
                bool present = queryWords.contains(word);
                if (!present && word.size() >= 7)
                    for (const auto& q : queryWords) if (q.size() >= 7 && q.left(word.size() - 2) == word.left(word.size() - 2)) present = true;
                if (present) ++matched;
                else if (QRegularExpression("\\d").match(word).hasMatch()) conflictingNumber = true;
            }
            matches = !conflictingNumber && significant >= 2 && matched >= 2 && matched * 4 >= significant * 3;
        }
        if (matches) found.append(document->id);
    }
    return found;
}

std::vector<ContextSource> DocumentContext::collect(const QString& question, const std::vector<const Document*>& documents,
        const std::vector<VectorStore::SearchResult>& hits, const QStringList& scope) {
    std::vector<ContextSource> result;
    QSet<QString> included;
    auto addRange = [&](const Document& document, Range range, bool whole, bool required) {
        const auto key = document.id + QString(":%1:%2").arg(range.first).arg(range.last);
        if (included.contains(key)) return;
        included.insert(key);
        QString text = "Источник: " + document.sourcePath + "\n" +
            (whole ? QStringLiteral("Охват: весь извлечённый документ.\n") :
                     QStringLiteral("Охват: связанный фрагмент документа; остальные разделы могут отсутствовать.\n"));
        QStringList titles, images;
        bool containsTables = false;
        int page = -1;
        if (document.blocks.empty()) text += document.text;
        for (int i = range.first; i < range.last; ++i) {
            const auto& block = document.blocks[i];
            containsTables = containsTables || block.type == DocumentBlockType::Table;
            if (block.pageNumber > 0 && block.pageNumber != page) {
                page = block.pageNumber; text += QStringLiteral("\n[Страница %1]\n").arg(page);
            }
            if (heading(block)) titles.append(block.text.left(250));
            text += blockText(block) + "\n\n";
            if (!block.imagePath.isEmpty() && !images.contains(block.imagePath)) images.append(block.imagePath);
        }
        const auto summary = text.left(850) + QStringLiteral("\nСодержание разделов:\n") + titles.join('\n').left(2000) +
                QStringLiteral("\nПолный текст источника: %1 символов. Таблицы и примечания передаются целиком.").arg(text.size());
        result.push_back({text, {}, summary, key, required, containsTables});
        for (const auto& path : images)
            result.push_back({"Источник: " + document.sourcePath + QStringLiteral("\nИзображение связанного раздела."),
                              path, QStringLiteral("Изображение источника: ") + document.sourcePath, key, required});
    };
    const auto references = tableReferences(question);
    const bool wholeDocument = explicitlyWholeDocument(question);
    const bool specificTable = !references.isEmpty() && !wholeDocument;
    QSet<QString> complete;
    for (const auto* document : documents) {
        if (!ready(document) || !scope.contains(document->id)) continue;
        // "All fields of table 5.4.1" asks for a complete table, not the whole file.
        if (specificTable) continue;
        int images = 0;
        for (const auto& block : document->blocks) if (block.type == DocumentBlockType::Image) ++images;
        if (document->text.size() <= 14000 && images <= 2) {
            addRange(*document, {0, static_cast<int>(document->blocks.size())}, true, true);
            complete.insert(document->id);
        } else if (wholeDocument || exhaustive(question)) {
            // A whole-document request must not silently turn into a top-k excerpt.
            // Actual tokenizer budget validation will reject an oversized document explicitly.
            addRange(*document, {0, static_cast<int>(document->blocks.size())}, true, true);
            complete.insert(document->id);
        }
    }
    QSet<QString> targeted;
    if (!references.isEmpty()) for (const auto* document : documents) {
        if (!ready(document) || complete.contains(document->id) || (!scope.isEmpty() && !scope.contains(document->id))) continue;
        for (int i = 0; i < static_cast<int>(document->blocks.size()); ++i) {
            if (document->blocks[i].type != DocumentBlockType::Table) continue;
            const auto range = tableRange(*document, i);
            for (int j = range.first; j < i; ++j) if (references.contains(tableReferences(document->blocks[j].text).value(0))) {
                addRange(*document, range, false, true); targeted.insert(document->id); break;
            }
        }
    }
    for (const auto& hit : hits) {
        if (complete.contains(hit.chunk.documentId) || (specificTable && targeted.contains(hit.chunk.documentId))) continue;
        const Document* document = nullptr;
        for (const auto* candidate : documents) if (ready(candidate) && candidate->id == hit.chunk.documentId) document = candidate;
        if (!document || (!scope.isEmpty() && !scope.contains(document->id))) continue;
        const int table = nearbyTable(*document, hit.chunk.blockIndex);
        if (table >= 0) {
            addRange(*document, tableRange(*document, table), false, false);
        } else {
            const auto key = document->id + ":offset:" + QString::number(hit.chunk.startOffset);
            if (included.contains(key)) continue;
            included.insert(key);
            auto text = hit.chunk.sourceDescription() + "\n\n" + hit.chunk.text;
            if (hit.chunk.imagePath.isEmpty()) {
                // Include neighbouring paragraphs, while retaining the matched text even
                // when a chunk straddles two paragraphs or comes from older stored data.
                const int index = hit.chunk.blockIndex;
                for (int i = std::max(0, index - 1); index >= 0 && i <= index + 2 && i < static_cast<int>(document->blocks.size()); ++i) {
                    const auto& block = document->blocks[i];
                    if (block.type == DocumentBlockType::Text && block.text.size() <= 1500 && !text.contains(block.text))
                        text += "\n\n" + block.text;
                }
            }
            result.push_back({text, hit.chunk.imagePath, text.left(1600), key, false});
        }
        if (included.size() >= 8) break;
    }
    return result;
}
