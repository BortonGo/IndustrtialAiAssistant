#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QStringList>

#include <vector>


class EmbeddingClient final : public QObject {
    Q_OBJECT
    QNetworkAccessManager *manager_;
public:
    EmbeddingClient(QObject *parent = nullptr);
    void requestModels();
    void requestEmbedding(const QString& text);
    void requestEmbeddings(const QStringList& texts);

signals:
    void errorOccurred(const QString& message);
    void embeddingReady(const std::vector<double>& embedding);
    void embeddingsReady(const std::vector<std::vector<double>>& embeddings);
};

