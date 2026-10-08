#pragma once

#include "documents/document.hpp"
#include "llm/context_builder.hpp"
#include "rag/vector_store.hpp"

namespace DocumentContext {
// Only the originating chat's ready documents may be supplied here.
QStringList namedDocuments(const QString& question, const std::vector<const Document*>& documents);
std::vector<ContextSource> collect(const QString& question, const std::vector<const Document*>& documents,
                                  const std::vector<VectorStore::SearchResult>& hits, const QStringList& scope);
}
