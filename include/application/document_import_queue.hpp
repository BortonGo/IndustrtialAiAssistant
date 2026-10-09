#pragma once
#include "io/document_loader.hpp"
#include "rag/embedding_client.hpp"
#include "rag/vector_store.hpp"
#include "storage/storage_client.hpp"
#include <QJsonArray>
#include <QTimer>
#include <QHash>

// Imports own their reader and index. Foreground chat state is never borrowed.
class DocumentImportQueue final : public QObject {
    Q_OBJECT
public:
    DocumentImportQueue(StorageClient* storage, const DocumentExtractionConfig& config,
                        const ModelApiConfig& embeddings, QObject* parent = nullptr);
    void enqueue(const QString& chatId, const QString& path);
    void control(const QString& id, const QString& action);
    void setModelsReady(bool ready);
    void setForegroundActive(bool active);
    void refresh();
    void stop();
    bool isIdle() const;
    QString activeChatId() const;
    QJsonArray jobs() const { return jobs_; }
signals:
    void changed();
    void idle();
    void documentSaved(const QString& chatId, const QString& fileName);
    void logMessage(const QString& message);
    void errorOccurred(const QString& message);
private:
    enum class Phase { Idle, Claiming, Reading, Indexing, Checkpoint, Saving, Finishing };
    StorageClient* storage_;
    DocumentLoader reader_;
    DocumentLoader nativeReader_;
    EmbeddingClient embeddings_;
    qint64 maxFileSize_;
    QJsonArray jobs_;
    QJsonObject current_;
    Document document_;
    std::vector<Chunk> chunks_;
    QJsonArray entries_;
    QHash<QString, std::vector<double>> reusable_;
    std::size_t batchSize_ = 0;
    Phase phase_ = Phase::Idle;
    bool modelsReady_ = false, foreground_ = false, stopping_ = false;
    bool readingNative_ = false;
    QString stopStatus_, progress_;
    QTimer progressTimer_;
    quint64 refreshVersion_ = 0;
    void pump();
    void acceptDocument(const Document& document, const QJsonArray& entries = {}, bool restored = false);
    void nextBatch();
    void readNext();
    void saveBatch(const QJsonArray& batch);
    void finish(const QString& status, const QString& error = {});
    void release();
    void interrupt(const QString& status);
    void publish(const QJsonObject& job);
    void progress(const QString& text);
};
