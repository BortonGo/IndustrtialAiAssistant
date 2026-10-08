#include "storage/storage_codec.hpp"
#include "io/document_extractor.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <stdexcept>

QJsonObject documentToJson(const Document& document) {
    QJsonArray blocks;
    for (const auto& block : document.blocks) {
        QJsonObject value{{"type", block.type == DocumentBlockType::Image ? "image" : block.type == DocumentBlockType::Table ? "table" : "text"},
                          {"text",block.text},{"pageNumber",block.pageNumber},{"section",block.section}};
        if (block.tableIndex >= 0) value["tableIndex"] = block.tableIndex;
        if (block.tableRow >= 0) value["tableRow"] = block.tableRow;
        if (block.tableColumn >= 0) value["tableColumn"] = block.tableColumn;
        if (block.type == DocumentBlockType::Image) value["imagePath"] = block.imagePath;
        if (block.type == DocumentBlockType::Table) {
            QJsonArray rows;
            for (const auto& row : block.tableRows) rows.append(QJsonArray::fromStringList(row));
            value["tableRows"] = rows;
        }
        blocks.append(value);
    }
    if (blocks.isEmpty()) blocks.append(QJsonObject{{"type","text"},{"text",document.text},{"pageNumber",0}});
    return {{"id",document.id},{"sourcePath",document.sourcePath},{"text",document.text},
            {"blocks",blocks},{"warnings",QJsonArray::fromStringList(document.warnings)}};
}

Document documentFromJson(const QJsonObject& value) {
    Document result;
    QString error;
    if (!parseDocumentContent(QJsonDocument(value).toJson(), result, error)) throw std::runtime_error(error.toUtf8().constData());
    result.id = value.value("id").toString();
    result.sourcePath = value.value("sourcePath").toString();
    const auto status = value.value("status").toString();
    result.status = status == "Ready" ? DocumentStatus::Ready : status == "ReadyWithWarnings" ? DocumentStatus::ReadyWithWarnings : DocumentStatus::Error;
    return result;
}

QJsonObject entryToJson(const VectorStore::Entry& entry) {
    const auto& c = entry.chunk;
    QJsonObject chunk{{"documentId",c.documentId},{"sourcePath",c.sourcePath},{"text",c.text},{"startOffset",c.startOffset},
                      {"pageNumber",c.pageNumber},{"blockIndex",c.blockIndex},{"tableIndex",c.tableIndex},
                      {"tableRow",c.tableRow},{"tableColumn",c.tableColumn},{"imagePath",c.imagePath},{"section",c.section}};
    QJsonArray vector;
    for (double x : entry.embedding) vector.append(x);
    return {{"chunk",chunk},{"embedding",vector}};
}

VectorStore::Entry entryFromJson(const QJsonObject& value) {
    VectorStore::Entry entry;
    auto& c = entry.chunk;
    const auto o = value.value("chunk").toObject();
    c.documentId = o.value("documentId").toString(); c.sourcePath = o.value("sourcePath").toString();
    c.text = o.value("text").toString(); c.startOffset = o.value("startOffset").toInt();
    c.pageNumber = o.value("pageNumber").toInt(); c.blockIndex = o.value("blockIndex").toInt(-1);
    c.tableIndex = o.value("tableIndex").toInt(-1); c.tableRow = o.value("tableRow").toInt(-1);
    c.tableColumn = o.value("tableColumn").toInt(-1); c.imagePath = o.value("imagePath").toString();
    c.section = o.value("section").toString();
    for (const auto& x : value.value("embedding").toArray()) {
        if (!x.isDouble()) throw std::runtime_error("Invalid saved embedding");
        entry.embedding.push_back(x.toDouble());
    }
    return entry;
}
