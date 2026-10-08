#include "rag/document_context.hpp"
#include "llm/context_selection.hpp"
#include <QCoreApplication>
#include <QDebug>
#include <stdexcept>

static void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
static DocumentBlock paragraph(const QString& text) { DocumentBlock block; block.text = text; return block; }
static DocumentBlock table(int index, const QString& marker) {
    DocumentBlock block; block.type = DocumentBlockType::Table; block.tableIndex = index;
    block.tableRows = {{QStringLiteral("Поле"), QStringLiteral("Размер"), QStringLiteral("Байт")},
                      {"HEADER", "32", "1"}, {marker, "16", "33"}};
    return block;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        Document document; document.id = "target"; document.status = DocumentStatus::Ready;
        document.sourcePath = QStringLiteral("C:/files/Дополнение № 4_v3 к ПФВ_note .docx");
        document.blocks = {paragraph(QStringLiteral("Введение")),
            paragraph(QStringLiteral("5.4.1 Структура первой посылки")), paragraph(QStringLiteral("Таблица 5.4.1")),
            table(0, "LAST_FIELD_A"), paragraph(QStringLiteral("Примечания: пакет 35 байт, период 100 мс.")),
            paragraph(QStringLiteral("5.4.2 Структура второй посылки")), paragraph(QStringLiteral("Таблица 5.4.2")),
            table(1, "LAST_FIELD_B"), paragraph(QStringLiteral("Примечания: квитанция 24 байта."))};
        for (const auto& block : document.blocks) document.text += block.indexText() + "\n";
        auto other = document; other.id = "other";
        other.sourcePath = QStringLiteral("C:/files/Дополнение № 5_v3 к ПФВ_note.docx");
        std::vector<const Document*> documents{&document, &other};
        const auto question = QStringLiteral("В дополнении №4_v3 к ПФВ_note.docx распиши все посылки и каждое поле.");
        const auto scope = DocumentContext::namedDocuments(question, documents);
        require(scope == QStringList{"target"}, "Filename normalization mixed document versions");
        require(DocumentContext::namedDocuments(QStringLiteral("Что такое протокол?"), documents).isEmpty(), "Generic question was scoped to a file");

        VectorStore::InMemoryVectorStore index;
        Chunk chunk; chunk.documentId = document.id; chunk.sourcePath = document.sourcePath;
        chunk.blockIndex = 1; chunk.text = QStringLiteral("Структура сообщения приведена в таблице 5.4.1");
        auto foreign = chunk; foreign.documentId = other.id;
        index.add({foreign, {1, 0}}); index.add({chunk, {0.8, 0.2}});
        const auto hits = index.search({1, 0}, 12, scope);
        require(hits.size() == 1 && hits[0].chunk.documentId == document.id, "Higher-scoring other document escaped scope");
        auto replacementIndex = index;
        auto replacementChunk = chunk; replacementChunk.text = "New complete index";
        for (const auto& invalid : {VectorStore::Entry{replacementChunk, {1.0}},
                                    VectorStore::Entry{replacementChunk, {0.0, 0.0}},
                                    VectorStore::Entry{foreign, {1.0, 0.0}}}) {
            bool rejected = false;
            try { replacementIndex.replaceDocument(document.id, {invalid}); }
            catch (const std::invalid_argument&) { rejected = true; }
            require(rejected && replacementIndex.documentEntries(document.id).size() == 1 &&
                    replacementIndex.documentEntries(document.id)[0].chunk.text == chunk.text,
                    "invalid replacement damaged a searchable index");
        }
        replacementIndex.replaceDocument(document.id, {{replacementChunk, {1.0, 0.0}}});
        require(replacementIndex.documentEntries(document.id).size() == 1 &&
                replacementIndex.documentEntries(document.id)[0].chunk.text == "New complete index" &&
                replacementIndex.documentEntries(other.id).size() == 1, "replacement duplicated chunks or changed another document");
        const auto full = DocumentContext::collect(question, documents, hits, scope);
        require(full.size() == 1 && full[0].required && full[0].text.contains("LAST_FIELD_A") && full[0].text.contains("LAST_FIELD_B") &&
                full[0].text.contains(QStringLiteral("24 байта")), "Named small document lost tables or notes");

        const auto targeted = DocumentContext::collect(QStringLiteral("Объясни таблицу 5.4.1"), {&document}, {}, {});
        require(!targeted.empty() && targeted[0].text.contains("LAST_FIELD_A") && targeted[0].text.contains(QStringLiteral("35 байт")) &&
                !targeted[0].text.contains("LAST_FIELD_B"), "Exact table reference lost its rows/notes or included the next table");
        auto large = document;
        large.blocks.push_back(paragraph(QStringLiteral("6.1 Приложение")));
        large.blocks.push_back(paragraph(QString("UNRELATED_SECTION ").repeated(4000)));
        large.text += large.blocks.back().text;
        auto unrelatedHit = chunk; unrelatedHit.blockIndex = 7;
        for (const auto* sized : {&document, &large}) {
            for (const auto& query : {QStringLiteral("Объясни все поля таблицы 5.4.1"),
                                      QStringLiteral("Объясни таблицу 5.4.1 полностью"),
                                      QStringLiteral("Explain every field of table 5.4.1")}) {
                const auto selectedTable = DocumentContext::collect(query, {sized}, {{unrelatedHit, 1.0}}, scope);
                require(selectedTable.size() == 1 && selectedTable[0].required &&
                        selectedTable[0].text.contains("LAST_FIELD_A") &&
                        !selectedTable[0].text.contains("LAST_FIELD_B") && !selectedTable[0].text.contains("UNRELATED_SECTION"),
                        "Request for all table fields included unrelated sections");
            }
        }
        const auto whole = DocumentContext::collect(QStringLiteral("Разбери весь документ, включая таблицу 5.4.1"), {&large}, {}, scope);
        require(whole.size() == 1 && whole[0].required && whole[0].text.contains("UNRELATED_SECTION"),
                "Explicit whole-document request was narrowed to one table");
        const auto wholeEnglish = DocumentContext::collect("Explain the whole document including table 5.4.1", {&large}, {}, scope);
        require(wholeEnglish.size() == 1 && wholeEnglish[0].text.contains("UNRELATED_SECTION"), "Whole-document intent lost in English");
        for (const auto& query : {QStringLiteral("Все поля таблиц 5.4.1 и 5.4.2"),
                                  QStringLiteral("All fields of tables 5.4.1 and 5.4.2")}) {
            const auto tables = DocumentContext::collect(query, {&large}, {}, scope);
            require(tables.size() == 2 && tables[0].text.contains("LAST_FIELD_A") && tables[1].text.contains("LAST_FIELD_B") &&
                    !tables[1].text.contains("UNRELATED_SECTION"), "Multiple exact table references lost a requested table");
        }
        auto singleNumber = document;
        singleNumber.blocks[2].text = QStringLiteral("Таблица 1");
        const auto firstTable = DocumentContext::collect(QStringLiteral("Все поля таблицы 1"), {&singleNumber}, {}, scope);
        require(firstTable.size() == 1 && firstTable[0].text.contains("LAST_FIELD_A") && !firstTable[0].text.contains("LAST_FIELD_B"),
                "Simple table number was not scoped");
        const auto expanded = DocumentContext::collect(QStringLiteral("Какие поля у первой посылки?"), {&document}, {{chunk, 1.0}}, {});
        require(!expanded.empty() && expanded[0].text.contains("LAST_FIELD_A"), "Heading hit was not expanded to its table");
        auto rowHit = chunk; rowHit.blockIndex = 3; rowHit.tableIndex = 0;
        const auto deduplicated = DocumentContext::collect("fields", {&document}, {{chunk,1.0},{rowHit,0.9}}, {});
        require(deduplicated.size() == 1, "Same table included multiple times");

        auto image = paragraph(QStringLiteral("Описание схемы")); image.type = DocumentBlockType::Image; image.imagePath = "scheme.png";
        auto imageDocument = document;
        imageDocument.blocks = {image, paragraph(QStringLiteral("Таблица 1")), table(0, "UNRELATED_TABLE")};
        auto imageHit = chunk; imageHit.blockIndex = 0; imageHit.imagePath = image.imagePath;
        const auto imageSources = DocumentContext::collect(QStringLiteral("Объясни схему"), {&imageDocument}, {{imageHit, 1.0}}, {});
        require(imageSources.size() == 1 && imageSources[0].imagePath == image.imagePath,
                "Adjacent table replaced a matched image");
        document.blocks.insert(document.blocks.begin() + 5, image);
        const auto withImage = DocumentContext::collect(question, {&document}, hits, scope);
        require(withImage.size() == 2 && withImage[0].groupId == withImage[1].groupId, "Document lost related image grouping");
        GenerationRequest request; request.question = question; request.sources = withImage;
        PreparedContext offered; offered.sourceCount = 2;
        const auto selected = selectContext(request, offered,
            "{\"context\":\"documents\",\"sources\":[0],\"history\":[],\"system_status\":false}");
        require(selected.sources.size() == 2, "Selection severed an image from its section");
        const auto unrelated = selectContext(request, offered,
            "{\"context\":\"conversation\",\"sources\":[0],\"history\":[],\"system_status\":false}");
        require(unrelated.sources.empty(), "Pinned document leaked into ordinary conversation");
        require(!DocumentContext::collect(question, {&other}, {}, {"target"}).size(), "Document from outside the origin chat was included");
        other.status = DocumentStatus::Error;
        require(DocumentContext::namedDocuments(other.sourcePath, {&other}).isEmpty(), "Failed document was treated as ready");
        qInfo() << "Document scope, table expansion, notes, deduplication, grouping and chat isolation passed";
        return 0;
    } catch (const std::exception& error) { qCritical() << error.what(); return 1; }
}
