#pragma once

#include "illm_client.hpp"

#include <QNetworkAccessManager>

class LMStudioLLMClient final : public ILLMClient {
    QNetworkAccessManager* manager_ = nullptr;
public:
    explicit LMStudioLLMClient(QObject* parent = nullptr);

    void generate(const QString& context,
                          const QString& question) override;
};
