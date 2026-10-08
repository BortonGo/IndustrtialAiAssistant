#pragma once

#include "context_builder.hpp"

// Current machine telemetry is opt-in per question, never enabled by an old answer.
bool requestsSystemStatus(const QString& question, const std::vector<ConversationTurn>& history = {});
QString withoutSystemStatus(const QString& text, const QString& documentReferences = {});
GenerationRequest contextWithoutUnrequestedStatus(const GenerationRequest& request);
