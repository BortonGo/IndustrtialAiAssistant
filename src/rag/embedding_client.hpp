#pragma once

#include <QObject>
#include <QNetworkAccessManager>


class EmbeddingClient final : public QObject {
    Q_OBJECT
    QNetworkAccessManager *manager_;
public:
    EmbeddingClient(QObject *parent = nullptr);
    void requestModels();
    void requestEmbedding(const QString& text);

signals:
    void errorOccurred(const QString& message);
    void embeddingReady(const std::vector<double>& message);
};

