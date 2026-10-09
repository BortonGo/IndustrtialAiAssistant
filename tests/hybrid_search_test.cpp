#include "rag/vector_store.hpp"
#include "rag/document_context.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <cstdio>
#include <limits>
#include <stdexcept>

using namespace VectorStore;
static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static Entry entry(const QString& text, const std::vector<double>& vector = {1, 0}, const QString& document = "doc", int offset = 0) {
    Chunk chunk; chunk.documentId = document; chunk.sourcePath = document + ".pdf"; chunk.text = text;
    chunk.startOffset = offset; chunk.pageNumber = 7; chunk.blockIndex = 2;
    return {chunk, vector};
}
static bool contains(const std::vector<SearchResult>& hits, const QString& text) {
    for (const auto& hit : hits) if (hit.chunk.text == text) return true;
    return false;
}
static void exactCodes() {
    InMemoryVectorStore index;
    for (int i=0; i<120; ++i) index.add(entry(QString("PUMP-%1 pressure nominal").arg(4170+i), {1, 0}, "doc", i*500));
    const QString target = "PUMP-417 pressure 7.5 bar; CRC16 checksum.";
    index.add(entry(target, {0, 1}, "doc", 70000));
    require(!contains(index.search({1,0}, 12), target), "Adversarial fixture must miss under semantic-only ranking");
    for (const auto& query : {QStringLiteral("Какое давление у PUMP-417?"), QStringLiteral("pump ‑ 417"),
                              QStringLiteral("pump—417"), QStringLiteral("pump-\n417"), QStringLiteral("Поле crc16")}) {
        const auto hits = index.searchHybrid(query, {1,0}, 12);
        require(hits.front().chunk.text == target, "Exact identifier lost to semantic distractors / formatting");
        require(hits.front().chunk.startOffset == 70000 && hits.front().chunk.pageNumber == 7, "Hit provenance lost");
    }
    InMemoryVectorStore zero;
    zero.add(entry("CODE-123", {1,0})); zero.add(entry("CODE-00123", {0,1}));
    require(zero.searchHybrid("CODE-00123", {1,0}, 1)[0].chunk.text == "CODE-00123", "Leading zeros were discarded");
    InMemoryVectorStore latin;
    latin.add(entry(QStringLiteral("РМ-417"), {1,0})); latin.add(entry("PM-417", {0,1}));
    require(latin.searchHybrid("PM-417", {1,0}, 1)[0].chunk.text == "PM-417", "Latin and Cyrillic codes were conflated");
}
static void wordsAndSemantics() {
    InMemoryVectorStore index;
    index.add(entry(QStringLiteral("Описание оборудования и инструкции"), {1,0}));
    index.add(entry(QStringLiteral("Манометр измеряет давление жидкости"), {0.9,0.1}));
    index.add(entry(QStringLiteral("Поле payload_length: число байтов полезной нагрузки"), {0,1}));
    require(index.searchHybrid("payload_length", {1,0}, 1)[0].chunk.text.contains("payload_length"), "Field name not found");
    require(index.searchHybrid(QStringLiteral("полезной нагрузки"), {1,0}, 1)[0].chunk.text.contains("payload_length"), "Lexical channel not contributing");
    // No literal matches: keep the original semantic ranking, including all ties.
    for (const auto& query : {QStringLiteral("Зачем нужен прибор контроля напора?"), QStringLiteral("и в на"), QStringLiteral("   ")}) {
        const auto old = index.search({0.1,1}, 3), now = index.searchHybrid(query, {0.1,1}, 3);
        for (int i=0; i<3; ++i) require(old[i].chunk.text == now[i].chunk.text, "Semantic fallback changed");
    }
    require(contains(index.searchHybrid(QStringLiteral("Описание датчика напора"), {0.1,1}, 3),
                     QStringLiteral("Поле payload_length: число байтов полезной нагрузки")), "Semantic candidate dropped when keywords exist");
    InMemoryVectorStore phrases;
    phrases.add(entry(QStringLiteral("длина и данные в заголовке"), {1,0}));
    phrases.add(entry(QStringLiteral("Поле Длина данных содержит 16 бит"), {0,1}));
    require(phrases.searchHybrid(QStringLiteral("Объясни «длина данных»"), {1,0}, 1)[0].chunk.text.contains("16"), "Quoted phrase not prioritized");
    InMemoryVectorStore cpp;
    cpp.add(entry("C++17", {1,0})); cpp.add(entry("C++ reference", {0,1}));
    require(cpp.searchHybrid("\"C++\"", {1,0}, 1)[0].chunk.text == "C++ reference", "Quoted punctuation/boundary lost");
    InMemoryVectorStore yo;
    yo.add(entry(QStringLiteral("счетчик приёма кадров"), {0,1})); yo.add(entry(QStringLiteral("другой раздел"), {1,0}));
    require(yo.searchHybrid(QStringLiteral("счётчик приема"), {1,0}, 1)[0].chunk.text.contains(QStringLiteral("кадров")), "Russian yo normalization failed");
}
static void coverageAndScope() {
    InMemoryVectorStore index;
    for (int i=0; i<30; ++i) index.add(entry(QString("PUMP-417 section %1").arg(i), {1,0}, "a", i*500));
    index.add(entry("PUMP-418 parameters", {0,1}, "b"));
    const auto both = index.searchHybrid("Compare PUMP-417 and PUMP-418", {1,0}, 2);
    require(both.size() == 2 && contains(both, "PUMP-418 parameters"), "Comparison omitted the second identifier");
    const auto many = index.searchHybrid("Compare PUMP-417 and PUMP-418", {1,0}, 12);
    require(many[0].chunk.documentId != many[1].chunk.documentId, "Second identifier would be lost when context clips top-12 hits");
    const auto scoped = index.searchHybrid("PUMP-417 and PUMP-418", {1,0}, 12, {"b"});
    require(scoped.size() == 1 && scoped[0].chunk.documentId == "b", "Exact match escaped document/attachment scope");
    require(index.searchHybrid("PUMP-417", {1,0}, 12, {"missing"}).empty(), "Empty scope leaked hits");
    const auto count = index.searchHybrid("PUMP-417", {1,0}, 999).size();
    require(count == 31 && index.searchHybrid("PUMP-417", {1,0}, 0).empty(), "Result limit broken");
}
static void mutations() {
    InMemoryVectorStore index;
    index.add(entry("OLD-101", {1,0}, "a")); index.add(entry("OTHER-202", {0,1}, "b"));
    auto copy = index;
    bool rejected = false;
    try { index.replaceDocument("a", {entry("BAD-303", {1}, "a")}); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && index.searchHybrid("OLD-101", {0,1}, 1)[0].chunk.text == "OLD-101", "Failed replacement corrupted lexical index");
    index.replaceDocument("a", {entry("NEW-303", {1,0}, "a")});
    require(index.searchHybrid("NEW-303", {0,1}, 1)[0].chunk.text == "NEW-303", "Replacement not searchable");
    require(copy.searchHybrid("OLD-101", {0,1}, 1)[0].chunk.text == "OLD-101", "Copied index mutated");
    const auto restored = index.documentEntries("a");
    index.removeDocument("a");
    require(index.searchHybrid("NEW-303", {1,0}, 12).size() == 1, "Removed lexical entry survived");
    index.clear(); require(index.empty() && index.searchHybrid("NEW-303", {1,0}, 12).empty(), "Clear left keywords");
    for (const auto& value : restored) index.add(value);
    require(index.searchHybrid("NEW-303", {0,1}, 1)[0].chunk.text == "NEW-303", "Restored entries not searchable");
    for (const auto& vector : {std::vector<double>{0,0}, std::vector<double>{1}, std::vector<double>{1,std::numeric_limits<double>::infinity()}}) {
        rejected = false;
        try { index.searchHybrid("NEW-303", vector, 1); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "Invalid query vector accepted");
    }
}
static void tableContext() {
    Document document; document.id = "doc"; document.status = DocumentStatus::TextReady; document.pendingImages = 3;
    document.sourcePath = "protocol.pdf";
    DocumentBlock heading; heading.text = QStringLiteral("Таблица 5.4.1"); heading.pageNumber = 7;
    DocumentBlock table; table.type = DocumentBlockType::Table; table.tableIndex = 0; table.pageNumber = 7;
    table.tableRows = {{"FIELD", "BITS"}, {"CRC16", "16"}, {"PAYLOAD_LEN", "8"}};
    document.blocks = {heading, table}; document.text = heading.text + "\n" + table.indexText();
    InMemoryVectorStore index;
    auto hit = entry(table.indexText(), {0,1}); hit.chunk.blockIndex = 1; index.add(hit);
    auto noise = entry("CRC160 describes unrelated fields", {1,0}); noise.chunk.blockIndex = -1; index.add(noise);
    const auto hits = index.searchHybrid("CRC16", {1,0}, 1);
    const auto sources = DocumentContext::collect("CRC16", {&document}, hits, {});
    require(sources.size() == 1 && sources[0].text.contains("PAYLOAD_LEN") && sources[0].text.contains("BITS"), "Exact table hit lost rows or column headings");
    require(sources[0].text.contains(QStringLiteral("3 изображений ещё не распознано")), "Hybrid result lost partial-document warning");
    require(index.searchHybrid("5.4.1", {1,0}, 1).size() == 1, "Missing literal must retain semantic retrieval");
}
static void benchmark() {
    InMemoryVectorStore index;
    std::vector<double> vector(768, 0); vector[0] = 1;
    QElapsedTimer timer; timer.start();
    for (int i=0; i<10000; ++i) index.add(entry(QString("PUMP-%1 pressure and temperature. ").arg(i) + QString("Technical protocol field description. ").repeated(10), vector, "large", i*500));
    const auto load = timer.elapsed(); timer.restart();
    for (int i=0; i<20; ++i) require(index.searchHybrid("PUMP-7319 pressure", vector, 12).front().chunk.text.contains("PUMP-7319 "), "Large index lost exact match");
    std::printf("10000 chunks / 768 dimensions: build=%lld ms; hybrid mean=%.1f ms\n", load, timer.elapsed()/20.0);
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        exactCodes(); wordsAndSemantics(); coverageAndScope(); mutations(); tableContext();
        if (app.arguments().contains("--benchmark")) benchmark();
        std::puts("Hybrid retrieval: identifiers, phrases, semantic fallback, scope, coverage, lifecycle and table context passed");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
