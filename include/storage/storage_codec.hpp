#pragma once
#include "documents/document.hpp"
#include "rag/vector_store.hpp"
#include <QJsonObject>

QJsonObject documentToJson(const Document& document);
Document documentFromJson(const QJsonObject& value);
QJsonObject entryToJson(const VectorStore::Entry& entry);
VectorStore::Entry entryFromJson(const QJsonObject& value);
