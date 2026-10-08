#pragma once

#include "context_builder.hpp"

// The selector returns only IDs of existing evidence, never invented facts.
QJsonArray contextSelectionMessages(const GenerationRequest& request, int sources, int historyPairs);
QJsonObject contextSelectionSchema();
GenerationRequest selectContext(const GenerationRequest& request, const PreparedContext& offered,
                                const QString& response);
