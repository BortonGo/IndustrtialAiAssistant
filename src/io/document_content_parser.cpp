#include "document_extractor.hpp"
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QFileInfo>
#include <cmath>

bool parseDocumentContent(const QByteArray& json, Document& document, QString& error)
{
    auto fail = [&error](const QString& message) { error = message; return false; };
    QJsonParseError parseError;
    const auto parsed = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !parsed.isObject())
        return fail("Invalid document JSON: " + parseError.errorString());
    const auto root = parsed.object();
    if (!root.value("text").isString() || !root.value("blocks").isArray() ||
        !root.value("warnings").isArray()) return fail("Expected text, blocks and warnings");
    Document result;
    result.text = root.value("text").toString();
    for (const auto& warning : root.value("warnings").toArray()) {
        if (!warning.isString()) return fail("Warning must be a string");
        result.warnings.append(warning.toString());
    }
    for (const auto& value : root.value("blocks").toArray()) {
        if (!value.isObject()) return fail("Block must be an object");
        const auto object = value.toObject();
        DocumentBlock block;
        auto integer = [&object](const char* name, int& target, int minimum) {
            const auto v = object.value(name);
            if (v.isUndefined()) return true;
            const double number = v.toDouble(-2);
            if (!v.isDouble() || number < minimum || number > 2147483647 || std::floor(number) != number)
                return false;
            target = static_cast<int>(number);
            return true;
        };
        if (!integer("pageNumber", block.pageNumber, 0) || !integer("tableIndex", block.tableIndex, 0) ||
            !integer("tableRow", block.tableRow, 0) || !integer("tableColumn", block.tableColumn, 0))
            return fail("Invalid block position");
        if (object.contains("section") && !object.value("section").isString()) return fail("Invalid section");
        block.section = object.value("section").toString();
        const auto type = object.value("type").toString();
        if (type == "text" || type == "image") {
            if (!object.value("text").isString()) return fail("Block text must be a string");
            block.text = object.value("text").toString();
            if (type == "image") {
                block.type = DocumentBlockType::Image;
                if (!object.value("imagePath").isString()) return fail("Image path must be a string");
                block.imagePath = object.value("imagePath").toString();
                const QFileInfo image(block.imagePath);
                if (!image.isAbsolute() || !image.isFile() || !image.isReadable())
                    return fail("Image file is unavailable: " + block.imagePath);
            }
        } else if (type == "table") {
            block.type = DocumentBlockType::Table;
            if (!object.value("tableRows").isArray()) return fail("Table rows must be an array");
            for (const auto& row : object.value("tableRows").toArray()) {
                if (!row.isArray()) return fail("Table row must be an array");
                QStringList cells;
                for (const auto& cell : row.toArray()) {
                    if (!cell.isString()) return fail("Table cell must be a string");
                    cells.append(cell.toString());
                }
                block.tableRows.push_back(cells);
            }
        } else return fail("Unknown block type: " + type);
        result.blocks.push_back(block);
    }
    QStringList texts;
    for (const auto& block : result.blocks) {
        const auto text = block.indexText();
        if (!text.trimmed().isEmpty()) texts.append(text);
    }
    if (result.text.trimmed().isEmpty()) return fail("No indexable text in document");
    if (texts.join("\n") != result.text) return fail("Document text does not match its blocks");
    document = std::move(result);
    return true;
}
