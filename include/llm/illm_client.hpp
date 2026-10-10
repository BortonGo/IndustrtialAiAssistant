#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include "context_builder.hpp"

class ILLMClient : public QObject {
    Q_OBJECT
public:
    explicit ILLMClient(QObject* parent = nullptr);
    virtual void prepare(const GenerationRequest& request) = 0;
    virtual void planConversation(const QString& question, const QJsonObject& state, int contextSize, bool followup,
                                  std::function<void(const QJsonObject&, const QString&)> callback) = 0;
    virtual void generatePrepared() = 0;
    virtual void cancelRequests() = 0;
signals:
    void answerProgress(const QString& text);
    void contextPrepared(const QString& sources, const QString& budget, const QJsonArray& references);
    void answerReady(const QString &answer);
    void errorOccurred(const QString &message);
};
