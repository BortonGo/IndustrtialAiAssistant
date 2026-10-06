#include "embedding_client.hpp"

#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QByteArray>
#include <QTimer>
#include <QDebug>

#include <vector>
#include <cmath>

EmbeddingClient::EmbeddingClient(QObject *parent) : QObject(parent) {
    manager_ = new QNetworkAccessManager(this);
}

void EmbeddingClient::requestModels() {
    QNetworkRequest request(QUrl("http://127.0.0.1:1234/v1/models"));
    auto* reply = manager_->get(request);
    connect(reply, &QNetworkReply::finished,
            this, [this, reply](){
       reply->deleteLater();
       if (reply->error() != QNetworkReply::NoError) {
           emit errorOccurred(reply->errorString());
           return;
       } else {
           qDebug() << reply->readAll();
       }
    });
}

void EmbeddingClient::requestEmbedding(const QString& text) {
    QNetworkRequest request(QUrl("http://127.0.0.1:1234/v1/embeddings"));
    request.setHeader(QNetworkRequest::ContentTypeHeader,"application/json");
    QJsonObject body;
    body["model"] = QString("text-embedding-embeddinggemma-300m-qat");
    body["input"] = text;
    QByteArray bytes = QJsonDocument(body).toJson();
    auto* reply = manager_->post(request, bytes);
    auto* timer = new QTimer(reply);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout,
            reply, &QNetworkReply::abort);
    connect(reply, &QNetworkReply::finished,
            timer, &QTimer::stop);
    connect(reply, &QNetworkReply::finished,
            this, [this, reply](){
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit errorOccurred(reply->errorString());
            return;
        } else {
            const QByteArray response = reply->readAll();

            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(response, &parseError);
            if (parseError.error != QJsonParseError::NoError) {
                emit errorOccurred(parseError.errorString());
                return;
            }
            if (!doc.isObject()) {
                emit errorOccurred("Expected a JSON object");
                return;
            }

            const QJsonObject root = doc.object();
            const QJsonValue dataValue = root.value("data");

            if (!dataValue.isArray()) {
                emit errorOccurred("Data must be an array");
                return;
            }

            const QJsonArray data = dataValue.toArray();

            if (data.size() != 1 || !data.at(0).isObject()) {
                emit errorOccurred("Expected one embedding result");
                return;
            }
            const QJsonObject item = data.at(0).toObject();
            const QJsonValue embeddingValue = item.value("embedding");

            if (!embeddingValue.isArray()) {
                emit errorOccurred("Embedding value must be an array");
                return;
            }

             const QJsonArray embeddingData = embeddingValue.toArray();
             if (embeddingData.isEmpty()) {
                 emit errorOccurred("Expected one embedding result");
                 return;
             }

             std::vector<double> embedding;
             embedding.reserve(embeddingData.size());

             for (const auto& e : embeddingData) {
                 if (!e.isDouble()) {
                     emit errorOccurred("Embedding value must be double");
                     return;
                 }
                 double number = e.toDouble();
                 if (!std::isfinite(number)) {
                     emit errorOccurred("Embedding value must be finite");
                     return;
                 }
                 embedding.push_back(number);
             }
             emit embeddingReady(embedding);
       }
    });
    timer->start(30000);
}

void EmbeddingClient::requestEmbeddings(const QStringList& texts) {
    if (texts.empty()) {
        emit errorOccurred("Embedding batch must not be empty");
        return;
    }

    QJsonArray input;
    for (const auto& text: texts) {
        input.append(text);
    }

    QJsonObject body;
    body["model"] = QString("text-embedding-embeddinggemma-300m-qat");
    body["input"] = input;

    QNetworkRequest request(QUrl("http://127.0.0.1:1234/v1/embeddings"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    auto* reply = manager_->post(request, QJsonDocument(body).toJson());

    auto* timer = new QTimer(reply);
    timer->setSingleShot(true);

    connect(timer, &QTimer::timeout, reply, &QNetworkReply::abort);
    connect(reply, &QNetworkReply::finished, timer, &QTimer::stop);

    const int expectedCount = texts.size();

    connect(reply, &QNetworkReply::finished,
            this, [this, reply, expectedCount]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            emit errorOccurred(reply->errorString());
            return;
        }

        const QByteArray response = reply->readAll();

        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(response, &parseError);

        if (parseError.error != QJsonParseError::NoError) {
            emit errorOccurred(parseError.errorString());
            return;
        }

        if (!doc.isObject()) {
            emit errorOccurred("Expected a JSON object");
            return;
        }

        const QJsonValue dataValue = doc.object().value("data");

        if (!dataValue.isArray()) {
            emit errorOccurred("Data must be an array");
            return;
        }

        const QJsonArray data = dataValue.toArray();

        if (data.size() != expectedCount) {
            emit errorOccurred("Embedding result count does not match input count");
            return;
        }

        std::vector<bool> seen(expectedCount, false);

        std::vector<std::vector<double>> embeddings(expectedCount);
        int expectedDimension = 0;

        for (const auto& value : data) {
            if (!value.isObject()) {
                emit errorOccurred("Embedding result must be an object");
                return;
            }

            const QJsonObject item = value.toObject();
            const int index = item.value("index").toInt(-1);

            if (index < 0 || index >= expectedCount) {
                emit errorOccurred("Embedding index is invalid");
                return;
            }

            if (seen[index]) {
                emit errorOccurred("Embedding index must be unique");
                return;
            }

            seen[index] = true;

            const QJsonValue embeddingValue = item.value("embedding");

            if (!embeddingValue.isArray()) {
                emit errorOccurred("Embedding must be an array");
                return;
            }

            const QJsonArray numbers = embeddingValue.toArray();

            if (numbers.isEmpty()) {
                emit errorOccurred("Embedding must not be empty");
                return;
            }

            if (expectedDimension != 0 && numbers.size() != expectedDimension) {
                emit errorOccurred("Embedding dimensions must match");
                return;
            }

            expectedDimension = numbers.size();

            auto& embedding = embeddings[index];
            embedding.reserve(numbers.size());

            for (const auto& number : numbers) {
                if (!number.isDouble() || !std::isfinite(number.toDouble())) {
                    emit errorOccurred("Embedding values must be finite numbers");
                    return;
                }

                embedding.push_back(number.toDouble());
            }
        }

        emit embeddingsReady(embeddings);
    });

    timer->start(30000);
}
