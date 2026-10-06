#include "assistant_service.hpp"
#include "document_chunker.h"
#include "embedding_client.hpp"
#include "lmstudio_llm_client.hpp"
#include "mcp_client.hpp"

#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QByteArray>

#include <utility>
#include <stdexcept>
#include <algorithm>

namespace {
    constexpr qint64 maxFileSize = 1024 * 1024;
    constexpr std::size_t embeddingBatchSize = 16;
}

AssistantService::AssistantService(QObject *parent) : QObject(parent) {

    // DocumentManager
    const QString pdfExtractorPath =
        "C:/Qt/IndustrialAiAssistant/tools/poppler-26.09.0/Library/bin/pdftotext.exe"; // TODO -> to settings in UI
    const QString docxPythonPath =
        "C:/Qt/IndustrialAiAssistant/document_tools/.venv/Scripts/python.exe"; // TODO -> to settings in UI
    const QString docxScriptPath =
        "C:/Qt/IndustrialAiAssistant/document_tools/read_docx.py"; // TODO -> to settings in UI

    documentManager_ = new DocumentManager(maxFileSize, pdfExtractorPath,
                                           docxPythonPath, docxScriptPath, this);

    connect(documentManager_, &DocumentManager::errorOccurred,
            this, &AssistantService::errorOccurred);

    connect(documentManager_, &DocumentManager::documentLoaded,
            this, [this](const QString& documentId) {
        indexingQueue_.push_back(documentId);
        tryStartNextDocument();
    });

    // EmbeddingClient
    embeddingClient_ = new EmbeddingClient(this);

    connect(embeddingClient_, &EmbeddingClient::errorOccurred,
            this, [this](const QString &message) {
        const bool wasQuerying = state_ == AssistantState::Querying;
        if (state_ == AssistantState::Indexing) {
            documentManager_->setDocumentStatus(indexingDocumentId_, DocumentStatus::Error);
            vectorStore_.removeDocument(indexingDocumentId_);
            indexingDocumentId_.clear();
        }
        pendingBatchSize_ = 0;
        state_ = AssistantState::Idle;
        busy_ = false;
        emit busyChanged(false);
        if (wasQuerying) {
            emit questionFailed(message);
            tryStartNextDocument();
            return;
        }
        emit errorOccurred(message);
        tryStartNextDocument();
    });
    connect(embeddingClient_, &EmbeddingClient::embeddingReady,
            this, [this](const std::vector<double> &embedding) {
        if (state_ == AssistantState::Querying) {
            try {
                const auto result = vectorStore_.search(embedding, 3);
                emit retrievalReady(result);
                QString context;
                for (const auto& r : result) {
                    context += "Источник: " + r.chunk.documentId + "\nПозиция: " + QString::number(r.chunk.startOffset) +
                            "\nSimilarity: " + QString::number(r.score) + "\n\n" + r.chunk.text + "\n\n";
                }
                if (!result.empty()) {
                    context += "\nПоказатели компьютера:\n" + buildSystemStatusContext();
                    state_ = AssistantState::Generating;
                    llmClient_->generate(context, pendingQuestion_);
                    return;
                } else {
                    state_ = AssistantState::Idle;
                    busy_ = false;
                    emit busyChanged(false);
                    emit questionFailed("Empty result");
                    tryStartNextDocument();
                    return;
                }
            } catch (const std::invalid_argument & e) {
                state_ = AssistantState::Idle;
                busy_ = false;
                emit busyChanged(false);
                emit questionFailed(QString::fromUtf8(e.what()));
                tryStartNextDocument();
                return;
            }
        }
    });

    connect(embeddingClient_, &EmbeddingClient::embeddingsReady,
            this, [this](const std::vector<std::vector<double>>& embeddings) {
        if (state_ != AssistantState::Indexing) {
            return;
        }

        try {
            if (pendingBatchSize_ == 0 ||
                embeddings.size() != pendingBatchSize_ ||
                nextChunkIndex_ > chunks_.size() ||
                pendingBatchSize_ > chunks_.size() - nextChunkIndex_) {
                throw std::invalid_argument("Unexpected embedding batch");
            }

            for (std::size_t i = 0; i < embeddings.size(); ++i) {
                VectorStore::Entry entry;
                entry.chunk = chunks_[nextChunkIndex_ + i];
                entry.embedding = embeddings[i];
                vectorStore_.add(entry);
            }
        } catch (const std::invalid_argument& e) {
            documentManager_->setDocumentStatus(
                indexingDocumentId_, DocumentStatus::Error);
            vectorStore_.removeDocument(indexingDocumentId_);

            indexingDocumentId_.clear();
            pendingBatchSize_ = 0;
            state_ = AssistantState::Idle;

            emit errorOccurred(QString::fromUtf8(e.what()));
            tryStartNextDocument();
            return;
        }

        nextChunkIndex_ += pendingBatchSize_;
        pendingBatchSize_ = 0;

        qDebug() << nextChunkIndex_ << "/" << chunks_.size();

        if (nextChunkIndex_ == chunks_.size()) {
            documentManager_->setDocumentStatus(
                indexingDocumentId_, DocumentStatus::Ready);

            indexingDocumentId_.clear();
            state_ = AssistantState::Idle;

            qDebug() << "Indexing finished";
            tryStartNextDocument();
            return;
        }

        requestNextEmbeddingBatch();
    });

    // LMStudioLLMClient
    llmClient_ = new LMStudioLLMClient(this);

    connect (llmClient_, &ILLMClient::answerReady,
             this, [this](const QString& answer) {
        state_ = AssistantState::Idle;
        busy_ = false;
        emit busyChanged(false);
        emit answerReady(answer);
        tryStartNextDocument();
    });

    connect (llmClient_, &ILLMClient::errorOccurred,
             this, [this](const QString& message) {
        state_ = AssistantState::Idle;
        busy_ = false;
        emit busyChanged(false);
        emit questionFailed(message);
        tryStartNextDocument();
    });

    // MCPClient
    auto* mcp_client = new MCPClient(this);

    connect(mcp_client, & MCPClient::errorOccurred,
            this, &AssistantService::errorOccurred);

    connect(mcp_client, &MCPClient::systemStatusReady,
            this, [this](double cpu_percent, double memory_percent) {
        cpuPercent_ = cpu_percent;
        memoryPercent_ = memory_percent;
        systemStatusReceivedAt_ = QDateTime::currentDateTimeUtc();
        emit systemStatusReady(cpu_percent, memory_percent);
    });

    mcp_client->start();

}

bool AssistantService::askQuestion(const QString &question) {
    if (question.trimmed().isEmpty()) {
        return false;
    }
    if (state_ != AssistantState::Idle) {
        return false;
    }
    if (busy_) {
        return false;
    }
    if (vectorStore_.empty()) {
        state_ = AssistantState::Generating;
        busy_ = true;
        emit busyChanged(true);
        llmClient_->generate(buildSystemStatusContext(), question);
        return true;
    }

    state_ = AssistantState::Querying;
    busy_ = true;
    emit busyChanged(true);
    pendingQuestion_ = question;
    embeddingClient_->requestEmbedding(question);
    return true;
}

void AssistantService::loadDocument(const QString& path) {
    documentManager_->loadFile(path);
}

DocumentManager* AssistantService::documentManager() const {
    return documentManager_;
}

QString AssistantService::buildSystemStatusContext() const {
    if (!systemStatusReceivedAt_.isValid()) {
        return "Computer data is not received yet";
    }
    auto diff = systemStatusReceivedAt_.secsTo(QDateTime::currentDateTimeUtc());
    if (diff < 0 || diff > 10) {
        return "Actual computer data is not available";
    }
    return QString("Источник: MCP get_system_status, локальный компьютер\n"
                   "Загрузка CPU: %1 %\nИспользование RAM: %2 %\n"
                   "Получено секунд назад: %3\n").arg(cpuPercent_).arg(memoryPercent_).arg(diff);
}

bool AssistantService::startDocumentIndexing(const QString& documentId) {
    const auto* d = documentManager_->findDocument(documentId);
    if (!d) {
        emit errorOccurred("Document not find");
        return false;
    }

    std::vector<Chunk> chunks;

    try {
        chunks = DocumentChunker::chunkDocument(*d, 500, 100);
    } catch (const std::invalid_argument &e) {
        documentManager_->setDocumentStatus(documentId, DocumentStatus::Error);
        emit errorOccurred(QString::fromUtf8(e.what()));
        return false;
    }

    chunks_ = std::move(chunks);

    qDebug() << chunks_.size();
    for (const auto& c : chunks_) {
        qDebug() << c.startOffset << c.text.size() << c.text.left(40);
    }

    emit documentLoaded(QFileInfo(d->sourcePath).fileName(), d->text.size());

    if (chunks_.empty()) {
        documentManager_->setDocumentStatus(documentId, DocumentStatus::Error);
        emit errorOccurred("Vector chunks_ is empty");
        return false;
    }
    indexingDocumentId_ = documentId;
    documentManager_->setDocumentStatus(indexingDocumentId_, DocumentStatus::Indexing);
    vectorStore_.removeDocument(indexingDocumentId_);
    nextChunkIndex_ = 0;
    pendingBatchSize_ = 0;
    state_ = AssistantState::Indexing;
    requestNextEmbeddingBatch();
    return true;
}

void AssistantService::tryStartNextDocument() {
    if (state_ != AssistantState::Idle) {
        return;
    }
    while (!indexingQueue_.empty()) {
        QString id = indexingQueue_.front();
        indexingQueue_.pop_front();
        if (startDocumentIndexing(id)) {
            return;
        }
    }
}

void AssistantService::requestNextEmbeddingBatch() {
    if (state_ != AssistantState::Indexing ||
        nextChunkIndex_ >= chunks_.size()) {
        return;
    }

    const std::size_t remaining = chunks_.size() - nextChunkIndex_;
    pendingBatchSize_ = std::min(embeddingBatchSize, remaining);

    QStringList texts;
    for (std::size_t i = 0; i < pendingBatchSize_; ++i) {
        texts.append(chunks_[nextChunkIndex_ + i].text);
    }

    embeddingClient_->requestEmbeddings(texts);
}

