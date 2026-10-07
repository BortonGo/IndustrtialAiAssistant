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

class LlamaServerProcess;
class QProcess;
class MCPClient;

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
    std::size_t nextChunkIndex_ = 0;
    std::size_t pendingBatchSize_ = 0;
    bool busy_ = false;

    double cpuPercent_ = 0.0;
    double memoryPercent_ = 0.0;
    QDateTime systemStatusReceivedAt_;

    std::deque<QString> indexingQueue_;
    QString runtimeRoot_;
    QString requestedBackend_;
    QString runtimeBackend_;
    QString chatModelPath_;
    QString embeddingModelPath_;
    int chatPort_ = 8080;
    int embeddingPort_ = 8081;
    int startupTimeoutMs_ = 120000;
    LlamaServerProcess* chatServer_ = nullptr;
    LlamaServerProcess* embeddingServer_ = nullptr;
    QProcess* backendProbe_ = nullptr;
    MCPClient* mcpClient_ = nullptr;
    bool serversStarted_ = false;
    bool shuttingDown_ = false;
    bool shutdownReported_ = false;
    bool reportedReady_ = false;
    QString chatServerStatus_ = QStringLiteral("Остановлен");
    QString embeddingServerStatus_ = QStringLiteral("Остановлен");

public:
    explicit AssistantService(QObject *parent = nullptr);
    ~AssistantService() override;
    void startServers();
    void stopServers();
    bool modelsReady() const;
    bool isBusy() const;
    QString chatServerStatus() const;
    QString embeddingServerStatus() const;
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
    void serverStatusChanged(const QString& server, const QString& text);
    void serverLogMessage(const QString& server, const QString& text);
    void modelsReadyChanged(bool ready);
    void serversStopped();

private:
    QString buildSystemStatusContext() const;
    bool startDocumentIndexing(const QString& documentId);
    void tryStartNextDocument();
    void requestNextEmbeddingBatch();
    void launchServers(bool useCuda);
    void updateModelsReady();
    void updateServersStopped();
    void handleServerFailure(const QString& message);
};
