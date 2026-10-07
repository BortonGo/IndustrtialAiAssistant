#pragma once

#include "illm_client.hpp"
#include "model_api_config.hpp"

#include <QNetworkAccessManager>

class LMStudioLLMClient final : public ILLMClient {
    QNetworkAccessManager* manager_ = nullptr;
    ModelApiConfig config_;
public:
    void cancelRequests() override;
    explicit LMStudioLLMClient(const ModelApiConfig& config, QObject* parent = nullptr);

    void generate(const QString& context,
                          const QString& question) override;
};
