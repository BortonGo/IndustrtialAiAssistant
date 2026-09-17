#pragma once

#include "io/document_manager.hpp"
#include "chunk.h"
#include "embedding_client.hpp"
#include "vector_store.hpp"
#include "illm_client.hpp"


#include <QObject>
#include <QString>
#include <QDateTime>

#include <vector>
#include <deque>

class AssistantService final : public QObject {
    Q_OBJECT

    enum class AssistantState {
        Idle,
        Indexing,
        Querying,
        Generating
    };

    DocumentManager* documentManager_;
    std::vector<Chunk> chunks_;
    QString indexingDocumentId_;

    ILLMClient* llmClient_ = nullptr;
    QString pendingQuestion_;

    EmbeddingClient* embeddingClient_ = nullptr;
    AssistantState state_ = AssistantState::Idle;

    VectorStore::InMemoryVectorStore vectorStore_;
    Chunk pendingChunk_;
    std::size_t nextChunkIndex_ = 0;
    bool busy_ = false;

    double cpuPercent_ = 0.0;
    double memoryPercent_ = 0.0;
    QDateTime systemStatusReceivedAt_;

    std::deque<QString> indexingQueue_;

public:
    explicit AssistantService(QObject *parent = nullptr);
    bool askQuestion(const QString &question);
    void loadDocument(const QString& path);
    DocumentManager* documentManager() const;

signals:
    void answerReady(const QString &answer);
    void errorOccurred(const QString &message);
    void questionFailed(const QString& message);
    void busyChanged(bool busy);
    void documentLoaded(const QString& fileName, int textlength);
    void retrievalReady(const std::vector<VectorStore::SearchResult>& results);
    void systemStatusReady(double cpuPercent, double memoryPercent);

private:
    QString buildSystemStatusContext() const;
    bool startDocumentIndexing(const QString& documentId);
    void tryStartNextDocument();
};
