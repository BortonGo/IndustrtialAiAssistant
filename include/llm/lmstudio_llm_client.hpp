#pragma once

#include "illm_client.hpp"
#include "model_api_config.hpp"

#include <QNetworkAccessManager>

class LMStudioLLMClient final : public ILLMClient {
    QNetworkAccessManager* manager_ = nullptr;
    ModelApiConfig config_;
    ContextBuilder* contextBuilder_ = nullptr;
    PreparedContext prepared_;
    bool preparedReady_ = false;
    quint64 version_ = 0;
    using CompletionCallback = std::function<void(const QString&, const QString&)>;
    void complete(const QJsonObject& body, CompletionCallback callback);
    void streamAnswer(const QJsonObject& body, CompletionCallback callback);
    void prepareAnswer(const GenerationRequest& request);
public:
    void cancelRequests() override;
    explicit LMStudioLLMClient(const ModelApiConfig& config, QObject* parent = nullptr);

    void prepare(const GenerationRequest& request) override;
    void generatePrepared() override;
};
