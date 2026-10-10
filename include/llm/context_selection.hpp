#pragma once

#include "context_builder.hpp"

// The selector returns only IDs of existing evidence, never invented facts.
QJsonArray contextSelectionMessages(const GenerationRequest& request, int sources, int historyPairs);
QJsonObject contextSelectionSchema();
GenerationRequest selectContext(const GenerationRequest& request, const PreparedContext& offered,
                                const QString& response);

bool needsFollowupQuery(const QString& question);
QJsonArray conversationPlanMessages(const QString& question, const QJsonObject& state, bool followup);
QJsonObject conversationPlanSchema();
QJsonObject validateConversationPlan(const QString& response, const QJsonObject& state, bool followup, const QString& question);
