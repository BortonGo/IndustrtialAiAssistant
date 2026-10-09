#include "assistant_service.hpp"
#include "application/document_import_queue.hpp"
#include "document_chunker.h"
#include "rag/document_context.hpp"
#include "embedding_client.hpp"
#include "lmstudio_llm_client.hpp"
#include "mcp_client.hpp"
#include "llama_server_process.hpp"
#include "runtime_paths.hpp"
#include "storage/storage_codec.hpp"
#include <QJsonArray>

#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QByteArray>
#include <QDir>
#include <QSettings>
#include <QProcess>
#include <QTimer>
#include <QCoreApplication>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <utility>
#include <stdexcept>
#include <algorithm>

namespace {
    constexpr std::size_t embeddingBatchSize = 16;
}

AssistantService::AssistantService(QObject *parent) : QObject(parent) {

    runtimeRoot_ = findRuntimeRoot();
    const QDir root(runtimeRoot_);
    QSettings settings(root.filePath("local-assistant.ini"), QSettings::IniFormat);
    settings.setIniCodec("UTF-8");
    requestedBackend_ = settings.value("models/backend", "auto").toString().toLower();
    const QString backendOverride = QString::fromLocal8Bit(qgetenv("LOCAL_ASSISTANT_BACKEND"));
    if (!backendOverride.isEmpty()) {
        requestedBackend_ = backendOverride.toLower();
    }
    chatPort_ = settings.value("models/chatPort", 8080).toInt();
    embeddingPort_ = settings.value("models/embeddingPort", 8081).toInt();
    startupTimeoutMs_ = settings.value("models/startupTimeoutMs", 120000).toInt();
    chatModelPath_ = root.filePath(settings.value("models/chatModel", "models/Qwen3-0.6B-Q8_0.gguf").toString());
    embeddingModelPath_ = root.filePath(settings.value("models/embeddingModel", "models/embeddinggemma-300m-qat-Q8_0.gguf").toString());
    const auto projector = settings.value("models/chatMmproj").toString();
    if (!projector.isEmpty()) projectorPath_ = root.filePath(projector);
    chatGpuLayers_ = settings.value("models/chatGpuLayers", "20").toString();
    chatKvCacheType_ = settings.value("models/chatKvCacheType", "f16").toString().trimmed().toLower();
    projectorOnGpu_ = settings.value("models/mmprojGpu", true).toBool();
    chatContextSize_ = qBound(4096, settings.value("models/chatContextSize", 8192).toInt(), 32768);
    visionParallelRequests_ = qBound(1, settings.value("documents/visionParallelRequests", 2).toInt(), 2);
    // Two image prompts and their descriptions must fit in the shared context pool.
    if (chatContextSize_ < 8192 || !QFileInfo(projectorPath_).isFile()) visionParallelRequests_ = 1;
    maxAnswerTokens_ = qBound(128, settings.value("models/maxAnswerTokens", 4096).toInt(), chatContextSize_ / 2);

    storage_ = new StorageClient(runtimeRoot_, embeddingModelPath_, this);
    connect(storage_, &StorageClient::statusChanged, this, [this](const QString& text) {
        emit serverStatusChanged("PostgreSQL", text);
        emit serverLogMessage(QStringLiteral("БД"), text);
        updateModelsReady();
    });
    connect(storage_, &StorageClient::unavailable, this, [this](const QString& error) {
        handleServerFailure(QStringLiteral("БД недоступна: ") + error);
    });
    connect(storage_, &StorageClient::stopped, this, &AssistantService::updateServersStopped);

    // DocumentManager
    DocumentExtractionConfig documentConfig;
    documentConfig.visionParallelRequests = visionParallelRequests_;
    documentConfig.pythonPath = root.filePath("document_tools/.venv/Scripts/python.exe");
    documentConfig.scriptPath = root.filePath("document_tools/read_document.py");
    documentConfig.popplerDirectory = root.filePath("tools/poppler-26.09.0/Library/bin");
    documentConfig.maxFileSize = qint64(qBound(1, settings.value("documents/maxFileSizeMiB", 100).toInt(), 1024)) * 1024 * 1024;
    const auto soffice = settings.value("documents/soffice").toString();
    if (!soffice.isEmpty()) documentConfig.sofficePath = root.filePath(soffice);
    if (QFileInfo(projectorPath_).isFile()) {
        documentConfig.visionUrl = QString("http://127.0.0.1:%1/v1/chat/completions").arg(chatPort_);
        for (const auto& path : {chatModelPath_, projectorPath_}) {
            const QFileInfo file(path);
            documentConfig.visionProfile += file.fileName() + QString::number(file.size()) +
                    file.lastModified().toUTC().toString(Qt::ISODate);
        }
    }
    documentManager_ = new DocumentManager(documentConfig, this);
    visibleDocuments_ = new DocumentManager(documentConfig, this);
    connect(documentManager_, &DocumentManager::documentLoaded, this, [this]() { syncVisibleDocuments(); });
    connect(documentManager_, &DocumentManager::documentStatusChanged, this, [this]() { syncVisibleDocuments(); });
    connect(this, &AssistantService::busyChanged, this, [this]() {
        if (state_ == AssistantState::Idle && !loadingChat_ && visibleChatId_ == activeChatId_) {
            visibleIndex_ = vectorStore_;
            syncVisibleDocuments();
        }
    });

    connect(documentManager_, &DocumentManager::errorOccurred, this, [this](const QString& message) {
        if (importingAttachments_) { failAttachment(message); return; }
        if (state_ == AssistantState::Reading) {
            state_ = AssistantState::Idle;
            busy_ = false;
            emit busyChanged(isBusy());
            emit documentProgress(message, false);
        }
        if (!shuttingDown_ && message != QStringLiteral("Обработка документа отменена")) emit errorOccurred(message);
    });
    connect(documentManager_, &DocumentManager::progressChanged, this, [this](const QString& message) {
        if (state_ != AssistantState::Reading || shuttingDown_) return;
        emit documentProgress(message, true);
        emit serverLogMessage(QStringLiteral("Документы"), message);
    });

    connect(documentManager_, &DocumentManager::documentIndexingRequested,
            this, [this](const QString& documentId) {
        state_ = AssistantState::Idle;
        busy_ = false;
        const auto* document = documentManager_->findDocument(documentId);
        if (document) for (const auto& warning : document->warnings)
            emit serverLogMessage(QStringLiteral("Документы"), warning);
        indexingQueue_.push_back(documentId);
        tryStartNextDocument();
    });

    // EmbeddingClient
    const ModelApiConfig embeddingConfig {
        QUrl(QString("http://127.0.0.1:%1/v1/").arg(embeddingPort_)),
        QString("embeddinggemma")
    };

    embeddingClient_ = new EmbeddingClient(embeddingConfig, this);
    auto importConfig = documentConfig;
    importConfig.resumeExtraction = true;
    imports_ = new DocumentImportQueue(storage_, importConfig, embeddingConfig, this);
    connect(imports_, &DocumentImportQueue::errorOccurred, this, &AssistantService::errorOccurred);
    connect(imports_, &DocumentImportQueue::logMessage, this, [this](const QString& message) {
        emit serverLogMessage(QStringLiteral("Импорт"), message);
    });
    connect(imports_, &DocumentImportQueue::idle, this, &AssistantService::continuePendingQuestion);
    connect(imports_, &DocumentImportQueue::documentSaved, this, [this](const QString& chatId, const QString& file) {
        reloadImportedDocuments(chatId);
        emit serverLogMessage(QStringLiteral("Импорт"), file + QStringLiteral(": доступная часть сохранена для поиска"));
    });
    connect(this, &AssistantService::busyChanged, this, [this]() {
        imports_->setForegroundActive(state_ != AssistantState::Idle || busy_);
    });

    connect(embeddingClient_, &EmbeddingClient::errorOccurred,
            this, [this](const QString &message) {
        const bool wasQuerying = state_ == AssistantState::Querying;
        if (state_ == AssistantState::Indexing) {
            emit documentProgress(QStringLiteral("Ошибка индексации"), false);
            finishDocumentIndexing(false);
        }
        pendingBatchSize_ = 0;
        state_ = AssistantState::Idle;
        busy_ = false;
        emit busyChanged(isBusy());
        if (importingAttachments_) { failAttachment(message); return; }
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
                std::vector<const Document*> documents;
                for (int i = 0; i < documentManager_->documentCount(); ++i) documents.push_back(documentManager_->documentAt(i));
                std::vector<ContextSource> sources;
                if (!attachedDocumentIds_.isEmpty()) {
                    for (const auto& id : attachedDocumentIds_) {
                        const QStringList scope{id};
                        const auto result = vectorStore_.searchHybrid(pendingQuestion_, embedding, 4, scope);
                        auto fileSources = DocumentContext::collect(pendingQuestion_, documents, result, scope);
                        if (fileSources.empty()) throw std::invalid_argument("Attached document has no searchable content");
                        const auto firstGroup = fileSources.front().groupId;
                        for (auto& source : fileSources) if (source.groupId == firstGroup) source.required = true;
                        sources.insert(sources.end(), fileSources.begin(), fileSources.end());
                    }
                } else {
                    const auto scope = DocumentContext::namedDocuments(pendingQuestion_, documents);
                    const auto result = vectorStore_.searchHybrid(pendingQuestion_, embedding, 12, scope);
                    emit retrievalReady(result);
                    sources = DocumentContext::collect(pendingQuestion_, documents, result, scope);
                }
                if (!sources.empty()) {
                    state_ = AssistantState::Generating;
                    prepareContext(sources);
                    return;
                } else {
                    state_ = AssistantState::Idle;
                    busy_ = false;
                    emit busyChanged(isBusy());
                    emit questionFailed("Empty result");
                    tryStartNextDocument();
                    return;
                }
            } catch (const std::invalid_argument & e) {
                state_ = AssistantState::Idle;
                busy_ = false;
                emit busyChanged(isBusy());
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
                pendingIndex_.add(entry);
            }
        } catch (const std::invalid_argument& e) {
            emit documentProgress(QStringLiteral("Ошибка индексации"), false);
            finishDocumentIndexing(false);
            state_ = AssistantState::Idle;

            emit busyChanged(isBusy());
            if (importingAttachments_) { failAttachment(QString::fromUtf8(e.what())); return; }
            emit errorOccurred(QString::fromUtf8(e.what()));
            tryStartNextDocument();
            return;
        }

        nextChunkIndex_ += pendingBatchSize_;
        pendingBatchSize_ = 0;

        qDebug() << nextChunkIndex_ << "/" << chunks_.size();

        if (nextChunkIndex_ == chunks_.size()) {
            const auto* document = documentManager_->findDocument(indexingDocumentId_);
            QJsonArray entries;
            for (const auto& entry : pendingIndex_.documentEntries(indexingDocumentId_)) entries.append(entryToJson(entry));
            const auto id = indexingDocumentId_;
            state_ = AssistantState::Saving;
            emit documentProgress(QStringLiteral("Сохранение документа и индекса в PostgreSQL…"), false);
            storage_->request("save_document", {{"chatId",activeChatId_},{"document",documentToJson(*document)},{"entries",entries}},
                              [this,id](const QJsonObject&, const QString& error) {
                if (shuttingDown_ || state_ != AssistantState::Saving || indexingDocumentId_ != id) return;
                finishDocumentIndexing(error.isEmpty());
                if (!error.isEmpty() && !importingAttachments_) emit errorOccurred(QStringLiteral("Документ не сохранён: ") + error);
                emit documentProgress(error.isEmpty() ? QStringLiteral("Документ сохранён и готов к поиску") : QStringLiteral("Ошибка сохранения документа"), false);
                indexingDocumentId_.clear(); state_ = AssistantState::Idle; busy_ = false;
                if (importingAttachments_) {
                    if (!error.isEmpty()) failAttachment(QStringLiteral("Документ не сохранён: ") + error);
                    else bindAttachment(id);
                    return;
                }
                emit busyChanged(isBusy()); tryStartNextDocument();
            });
            return;
        }

        requestNextEmbeddingBatch();
    });

    // LMStudioLLMClient
    const ModelApiConfig chatConfig {
        QUrl(QString("http://127.0.0.1:%1/v1/").arg(chatPort_)),
        QString("local-chat")
    };

    llmClient_ = new LMStudioLLMClient(chatConfig, this);
    connect(llmClient_, &ILLMClient::contextPrepared, this, [this](const QString& sources, const QString& budget) {
        if (shuttingDown_ || state_ != AssistantState::Generating) return;
        emit serverLogMessage(QStringLiteral("Контекст"), budget);
        const auto messageId = requestMessageId_;
        if (messageId.isEmpty()) { llmClient_->generatePrepared(); return; }
        storage_->request("save_context", {{"id",messageId},{"context",sources}},
                          [this,messageId,sources](const QJsonObject&, const QString& error) {
            if (shuttingDown_ || state_ != AssistantState::Generating || requestMessageId_ != messageId) return;
            if (error.isEmpty()) { emit contextSaved(messageId, sources); llmClient_->generatePrepared(); }
            else { state_ = AssistantState::Idle; busy_ = false; emit busyChanged(isBusy()); emit questionFailed(error); }
        });
    });

    connect(llmClient_, &ILLMClient::answerProgress, this, [this](const QString& text) {
        if (!shuttingDown_ && state_ == AssistantState::Generating) emit answerProgress(requestMessageId_, text);
    });

    connect (llmClient_, &ILLMClient::answerReady,
             this, [this](const QString& answer) {
        if (shuttingDown_ || state_ != AssistantState::Generating) return;
        state_ = AssistantState::Idle;
        busy_ = false;
        emit busyChanged(isBusy());
        emit answerReady(answer);
        tryStartNextDocument();
    });

    connect (llmClient_, &ILLMClient::errorOccurred,
             this, [this](const QString& message) {
        if (shuttingDown_ || state_ != AssistantState::Generating) return;
        state_ = AssistantState::Idle;
        busy_ = false;
        emit busyChanged(isBusy());
        emit questionFailed(message);
        tryStartNextDocument();
    });

    // MCPClient
    auto* mcp_client = new MCPClient(
        root.filePath("mcp_server/.venv/Scripts/python.exe"),
        root.filePath("mcp_server/server.py"), this);
    mcpClient_ = mcp_client;

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

bool AssistantService::askQuestion(const QString &question) { return askQuestion(question, {}); }

bool AssistantService::askQuestion(const QString &question, const QString& messageId, const QString& chatId, const QStringList& attachments) {
    if (!modelsReady()) {
        return false;
    }
    if (question.trimmed().isEmpty()) {
        return false;
    }
    if (state_ != AssistantState::Idle) {
        return false;
    }
    if (busy_) {
        return false;
    }
    requestMessageId_ = messageId;
    pendingAttachmentPaths_ = attachments; attachedDocumentIds_.clear(); nextAttachment_ = 0;
    importingAttachments_ = !attachments.isEmpty();
    pendingQuestion_ = question;
    const auto targetChat = chatId.isEmpty() ? visibleChatId_ : chatId;
    if (targetChat.isEmpty()) return false;
    state_ = AssistantState::Preparing;
    busy_ = true;
    pendingQuestionChatId_ = targetChat;
    waitingForImports_ = true;
    emit busyChanged(true);
    continuePendingQuestion();
    return true;
}

void AssistantService::continuePendingQuestion() {
    if (shuttingDown_ || !waitingForImports_ || state_ != AssistantState::Preparing || !imports_->isIdle()) return;
    waitingForImports_ = false;
    const auto targetChat = pendingQuestionChatId_, messageId = requestMessageId_;
    // Reload after the importer has released its job, including any just-committed index.
        storage_->request("load_chat", {{"chatId",targetChat},{"remember",false}},
                          [this,targetChat,messageId](const QJsonObject& data, const QString& error) {
            if (shuttingDown_ || state_ != AssistantState::Preparing || requestMessageId_ != messageId) return;
            QString failure = error;
            if (failure.isEmpty()) {
                try {
                    std::vector<Document> documents;
                    VectorStore::InMemoryVectorStore index;
                    for (const auto& value : data.value("documents").toArray()) documents.push_back(documentFromJson(value.toObject()));
                    for (const auto& value : data.value("entries").toArray()) index.add(entryFromJson(value.toObject()));
                    activeChatId_ = targetChat;
                    documentManager_->restore(std::move(documents)); vectorStore_ = std::move(index);
                } catch (const std::exception& e) { failure = QString::fromUtf8(e.what()); }
            }
            if (!failure.isEmpty()) { importingAttachments_ = false; state_ = AssistantState::Idle; busy_ = false; emit busyChanged(isBusy()); emit questionFailed(failure); return; }
            startQuestion();
        });
}

void AssistantService::reloadImportedDocuments(const QString& chatId) {
    const auto version = selectionVersion_;
    const bool visible = visibleChatId_ == chatId;
    if (visible) { loadingChat_ = true; emit busyChanged(isBusy()); }
    storage_->request("load_chat", {{"chatId",chatId},{"remember",false}},
        [this,chatId,version,visible](const QJsonObject& data, const QString& error) {
        if (shuttingDown_) return;
        try {
            if (!error.isEmpty()) throw std::runtime_error(error.toUtf8().constData());
            std::vector<Document> documents;
            VectorStore::InMemoryVectorStore index;
            for (const auto& value : data.value("documents").toArray()) documents.push_back(documentFromJson(value.toObject()));
            for (const auto& value : data.value("entries").toArray()) index.add(entryFromJson(value.toObject()));
            if (state_ == AssistantState::Idle && activeChatId_ == chatId) {
                documentManager_->restore(documents); vectorStore_ = index;
            }
            if (version == selectionVersion_ && visibleChatId_ == chatId) {
                visibleDocuments_->restore(std::move(documents)); visibleIndex_ = std::move(index);
            }
        } catch (const std::exception& failure) { emit errorOccurred(QString::fromUtf8(failure.what())); }
        if (visible && version == selectionVersion_) loadingChat_ = false;
        emit busyChanged(isBusy());
    });
}

void AssistantService::startQuestion() {
    if (importingAttachments_) { prepareNextAttachment(); return; }
    if (vectorStore_.empty()) {
        state_ = AssistantState::Generating;
        busy_ = true;
        emit busyChanged(true);
        prepareContext();
        return;
    }

    state_ = AssistantState::Querying;
    busy_ = true;
    emit busyChanged(true);
    embeddingClient_->requestEmbedding(pendingQuestion_);
}

void AssistantService::prepareNextAttachment() {
    if (nextAttachment_ >= pendingAttachmentPaths_.size()) {
        importingAttachments_ = false; startQuestion(); return;
    }
    const auto path = pendingAttachmentPaths_[nextAttachment_];
    state_ = AssistantState::Reading; busy_ = true;
    emit busyChanged(isBusy());
    emit documentProgress(QStringLiteral("Вложение %1/%2: %3").arg(nextAttachment_ + 1)
                          .arg(pendingAttachmentPaths_.size()).arg(QFileInfo(path).fileName()), true);
    // A file may have changed since its previous upload. Refresh its index;
    // the image-description cache still avoids repeated vision work.
    documentManager_->loadFile(path, true);
}

void AssistantService::bindAttachment(const QString& documentId) {
    state_ = AssistantState::Preparing; busy_ = true;
    const auto questionId = requestMessageId_;
    emit documentProgress(QStringLiteral("Сохранение вложения сообщения…"), false);
    storage_->request("bind_attachment", {{"questionId",questionId},{"position",nextAttachment_},{"documentId",documentId}},
        [this,questionId,documentId](const QJsonObject& data, const QString& error) {
        if (shuttingDown_ || !importingAttachments_ || questionId != requestMessageId_) return;
        if (!error.isEmpty()) { failAttachment(error); return; }
        attachedDocumentIds_.append(documentId); ++nextAttachment_;
        emit attachmentsSaved(questionId, data.value("attachments").toArray());
        prepareNextAttachment();
    });
}

void AssistantService::failAttachment(const QString& error) {
    importingAttachments_ = false;
    indexingQueue_.clear(); state_ = AssistantState::Idle; busy_ = false;
    emit busyChanged(isBusy());
    emit documentProgress(QStringLiteral("Обработка вложений остановлена"), false);
    emit questionFailed(QStringLiteral("Не удалось подготовить вложение: ") + error);
}

void AssistantService::prepareContext(const std::vector<ContextSource>& sources) {
    GenerationRequest request;
    request.question = pendingQuestion_; request.sources = sources;
    for (const auto& path : pendingAttachmentPaths_) request.attachedFiles.append(QFileInfo(path).fileName());
    request.systemStatus = buildSystemStatusContext();
    request.contextSize = chatContextSize_; request.maxAnswerTokens = maxAnswerTokens_;
    if (requestMessageId_.isEmpty()) {
        llmClient_->prepare(request);
        return;
    }
    const auto messageId = requestMessageId_;
    storage_->request("question_history", {{"questionId",messageId},{"chatId",activeChatId_}},
                      [this,request,messageId](const QJsonObject& data, const QString& error) mutable {
        if (shuttingDown_ || state_ != AssistantState::Generating || requestMessageId_ != messageId) return;
        if (error.isEmpty()) {
            for (const auto& value : data.value("pairs").toArray()) {
                const auto pair = value.toObject();
                request.history.push_back({pair.value("question").toString(), pair.value("answer").toString()});
            }
            llmClient_->prepare(request);
        }
        else { state_ = AssistantState::Idle; busy_ = false; emit busyChanged(isBusy()); emit questionFailed(error); }
    });
}

void AssistantService::activateChat(const QString& chatId, std::function<void(const QString&)> callback) {
    const auto version = ++selectionVersion_;
    visibleChatId_ = chatId; loadingChat_ = true;
    visibleDocuments_->restore({}); visibleIndex_ = {};
    emit busyChanged(true);
    const auto method = chatId == activeChatId_ ? "select_chat" : "load_chat";
    std::vector<Document> cachedDocuments;
    VectorStore::InMemoryVectorStore cachedIndex;
    const bool cached = chatId == activeChatId_;
    if (cached) {
        for (int i = 0; i < documentManager_->documentCount(); ++i) cachedDocuments.push_back(*documentManager_->documentAt(i));
        cachedIndex = vectorStore_;
    }
    storage_->request(method, {{"chatId",chatId}}, [this,chatId,version,callback,cached,cachedDocuments,cachedIndex](const QJsonObject& data, const QString& error) {
        if (shuttingDown_ || version != selectionVersion_) return;
        QString failure = error;
        if (failure.isEmpty()) {
            try {
                if (chatId == activeChatId_) {
                    syncVisibleDocuments(); visibleIndex_ = vectorStore_;
                } else if (cached) {
                    visibleDocuments_->restore(cachedDocuments); visibleIndex_ = cachedIndex;
                } else {
                    std::vector<Document> documents;
                    VectorStore::InMemoryVectorStore index;
                    for (const auto& value : data.value("documents").toArray()) documents.push_back(documentFromJson(value.toObject()));
                    for (const auto& value : data.value("entries").toArray()) index.add(entryFromJson(value.toObject()));
                    visibleDocuments_->restore(std::move(documents)); visibleIndex_ = std::move(index);
                }
            } catch (const std::exception& e) { failure = QString::fromUtf8(e.what()); }
        }
        loadingChat_ = false;
        if (!failure.isEmpty()) visibleChatId_.clear();
        emit busyChanged(isBusy()); updateModelsReady(); callback(failure);
        if (failure.isEmpty()) emit chatActivated(chatId);
    });
}

void AssistantService::forgetChat(const QString& id) {
    if (id == operationChatId()) return;
    if (activeChatId_ == id) {
        activeChatId_.clear(); documentManager_->restore({}); vectorStore_.clear();
        chunks_.clear(); pendingIndex_.clear(); indexingQueue_.clear(); indexingDocumentId_.clear();
    }
    if (visibleChatId_ == id) {
        ++selectionVersion_; visibleChatId_.clear(); loadingChat_ = false;
        visibleDocuments_->restore({}); visibleIndex_.clear();
    }
    imports_->refresh();
}

void AssistantService::syncVisibleDocuments() {
    if (visibleChatId_ != activeChatId_) return;
    std::vector<Document> documents;
    for (int i = 0; i < documentManager_->documentCount(); ++i) documents.push_back(*documentManager_->documentAt(i));
    visibleDocuments_->restore(std::move(documents));
}

QString AssistantService::operationChatId() const {
    if (state_ != AssistantState::Idle && state_ != AssistantState::Preparing) return activeChatId_;
    return imports_ ? imports_->activeChatId() : QString();
}

void AssistantService::loadDocument(const QString& path) {
    if (!storage_->isReady() || visibleChatId_.isEmpty() || loadingChat_) {
        emit errorOccurred(QStringLiteral("Дождитесь загрузки чата и готовности БД"));
        return;
    }
    imports_->enqueue(visibleChatId_, path);
}

void AssistantService::cancelDocument()
{
    if (state_ == AssistantState::Reading) {
        documentManager_->cancelLoading();
    } else if (state_ == AssistantState::Indexing) {
        embeddingClient_->cancelRequests();
        finishDocumentIndexing(false);
        state_ = AssistantState::Idle;
        busy_ = false;
        emit busyChanged(isBusy());
        emit documentProgress(QStringLiteral("Индексация отменена"), false);
        if (importingAttachments_) failAttachment(QStringLiteral("Индексация отменена"));
    }
}

DocumentManager* AssistantService::documentManager() const {
    return visibleDocuments_;
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

    indexingDocumentId_ = documentId;
    pendingIndex_.clear();

    std::vector<Chunk> chunks;

    try {
        chunks = DocumentChunker::chunkDocument(*d, 500, 100);
    } catch (const std::invalid_argument &e) {
        finishDocumentIndexing(false);
        emit errorOccurred(QString::fromUtf8(e.what()));
        return false;
    }

    chunks_ = std::move(chunks);

    if (chunks_.empty()) {
        finishDocumentIndexing(false);
        emit errorOccurred("Vector chunks_ is empty");
        return false;
    }
    indexingDocumentId_ = documentId;
    documentManager_->setDocumentStatus(indexingDocumentId_, DocumentStatus::Indexing);
    nextChunkIndex_ = 0;
    pendingBatchSize_ = 0;
    state_ = AssistantState::Indexing;
    emit documentLoaded(QFileInfo(d->sourcePath).fileName(), d->text.size());
    emit busyChanged(true);
    requestNextEmbeddingBatch();
    return true;
}

void AssistantService::finishDocumentIndexing(bool success) {
    const auto id = indexingDocumentId_;
    if (success) vectorStore_.replaceDocument(id, pendingIndex_.documentEntries(id));
    pendingIndex_.clear();
    chunks_.clear();
    indexingDocumentId_.clear();
    pendingBatchSize_ = 0;
    if (!id.isEmpty()) documentManager_->finishIndexing(id, success);
}

void AssistantService::tryStartNextDocument() {
    if (state_ != AssistantState::Idle || !modelsReady()) {
        return;
    }
    while (!indexingQueue_.empty()) {
        QString id = indexingQueue_.front();
        indexingQueue_.pop_front();
        if (startDocumentIndexing(id)) {
            return;
        }
        if (importingAttachments_) { failAttachment(QStringLiteral("Не удалось построить индекс вложения")); return; }
    }
    emit busyChanged(isBusy());
}

void AssistantService::requestNextEmbeddingBatch() {
    if (!modelsReady() || state_ != AssistantState::Indexing ||
        nextChunkIndex_ >= chunks_.size()) {
        return;
    }

    const std::size_t remaining = chunks_.size() - nextChunkIndex_;
    emit documentProgress(QStringLiteral("Индексация: %1/%2 фрагментов")
                          .arg(static_cast<qulonglong>(nextChunkIndex_)).arg(static_cast<qulonglong>(chunks_.size())), true);
    pendingBatchSize_ = std::min(embeddingBatchSize, remaining);

    QStringList texts;
    for (std::size_t i = 0; i < pendingBatchSize_; ++i) {
        texts.append(chunks_[nextChunkIndex_ + i].text);
    }

    embeddingClient_->requestEmbeddings(texts);
}


AssistantService::~AssistantService()
{
    shuttingDown_ = true;
    documentManager_->cancelLoading();
    if (backendProbe_) {
        backendProbe_->disconnect(this);
        if (backendProbe_->state() != QProcess::NotRunning) {
            backendProbe_->kill();
            backendProbe_->waitForFinished(1000);
        }
    }
    embeddingClient_->cancelRequests();
    llmClient_->cancelRequests();
    delete chatServer_;
    chatServer_ = nullptr;
    delete embeddingServer_;
    embeddingServer_ = nullptr;
}

bool AssistantService::modelsReady() const
{
    return !shuttingDown_ && storage_->isReady() && chatServer_ && embeddingServer_ &&
           chatServer_->isReady() && embeddingServer_->isReady();
}

bool AssistantService::isBusy() const
{
    return busy_ || state_ != AssistantState::Idle || loadingChat_;
}

QString AssistantService::chatServerStatus() const { return chatServerStatus_; }
QString AssistantService::embeddingServerStatus() const { return embeddingServerStatus_; }

void AssistantService::updateModelsReady()
{
    const bool ready = modelsReady();
    if (imports_) imports_->setModelsReady(ready);
    if (ready != reportedReady_) {
        reportedReady_ = ready;
        emit modelsReadyChanged(ready);
    }
}

void AssistantService::startServers()
{
    if (serversStarted_ || shuttingDown_) {
        return;
    }
    serversStarted_ = true;
    storage_->start();
    emit serverLogMessage("Запуск", QStringLiteral("Папка данных: %1").arg(runtimeRoot_));
    if (chatPort_ == embeddingPort_ || chatPort_ < 1 || chatPort_ > 65535 ||
        embeddingPort_ < 1 || embeddingPort_ > 65535 || startupTimeoutMs_ <= 0 ||
        (requestedBackend_ != "auto" && requestedBackend_ != "cpu" && requestedBackend_ != "cuda")) {
        chatServerStatus_ = embeddingServerStatus_ = QStringLiteral("Ошибка: проверьте local-assistant.ini");
        emit serverStatusChanged("Qwen", chatServerStatus_);
        emit serverStatusChanged("EmbeddingGemma", embeddingServerStatus_);
        emit errorOccurred(chatServerStatus_);
        return;
    }
    const QString cudaExecutable = QDir(runtimeRoot_).filePath("tools/llama-cpp/cuda/llama-server.exe");
    if (requestedBackend_ == "cpu" || (requestedBackend_ == "auto" && !QFileInfo::exists(cudaExecutable))) {
        launchServers(false);
        return;
    }
    if (requestedBackend_ == "cuda") {
        launchServers(true);
        return;
    }

    chatServerStatus_ = embeddingServerStatus_ = QStringLiteral("Проверка CUDA…");
    emit serverStatusChanged("Qwen", chatServerStatus_);
    emit serverStatusChanged("EmbeddingGemma", embeddingServerStatus_);
    backendProbe_ = new QProcess(this);
    backendProbe_->setProcessChannelMode(QProcess::MergedChannels);
    backendProbe_->setWorkingDirectory(QFileInfo(cudaExecutable).absolutePath());
#ifdef Q_OS_WIN
    backendProbe_->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags |= CREATE_NO_WINDOW;
    });
#endif
    connect(backendProbe_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        updateServersStopped();
        if (error == QProcess::FailedToStart && !chatServer_ && !shuttingDown_) {
            emit serverLogMessage("Запуск", QStringLiteral("CUDA недоступна, используем CPU"));
            launchServers(false);
        }
    });
    connect(backendProbe_,
            static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus status) {
        updateServersStopped();
        if (chatServer_ || shuttingDown_) {
            return;
        }
        const QString devices = QString::fromUtf8(backendProbe_->readAllStandardOutput());
        emit serverLogMessage("Запуск", devices.trimmed());
        launchServers(exitCode == 0 && status == QProcess::NormalExit && devices.contains("CUDA0:"));
    });
    QTimer::singleShot(5000, backendProbe_, [this]() {
        if (!chatServer_ && !shuttingDown_) {
            backendProbe_->kill();
            emit serverLogMessage("Запуск", QStringLiteral("Проверка CUDA не завершилась, используем CPU"));
            launchServers(false);
        }
    });
    backendProbe_->start(cudaExecutable, QStringList() << "--list-devices");
}

void AssistantService::launchServers(bool useCuda)
{
    if (chatServer_ || embeddingServer_ || shuttingDown_) {
        return;
    }
    runtimeBackend_ = useCuda ? QStringLiteral("CUDA") : QStringLiteral("CPU");
    const QDir root(runtimeRoot_);
    const QString executable = root.filePath(useCuda ? "tools/llama-cpp/cuda/llama-server.exe"
                                                     : "tools/llama-cpp/llama-server.exe");
    const QString gpuLayers = useCuda ? QStringLiteral("all") : QStringLiteral("0");
    LlamaServerConfig chatConfig;
    chatConfig.executablePath = executable;
    chatConfig.healthUrl = QUrl(QString("http://127.0.0.1:%1/health").arg(chatPort_));
    chatConfig.startupTimeoutMs = startupTimeoutMs_;
    chatConfig.arguments = QStringList() << "--model" << chatModelPath_ << "--alias" << "local-chat"
        << "--host" << "127.0.0.1" << "--port" << QString::number(chatPort_)
        << "--ctx-size" << QString::number(chatContextSize_) << "--parallel" << QString::number(visionParallelRequests_)
        << "--gpu-layers" << (useCuda ? chatGpuLayers_ : QString("0"))
        << "--jinja" << "--reasoning" << "off" << "--cache-ram" << "0";
    if (visionParallelRequests_ > 1) {
        // Share the existing KV allocation; a later chat request still has the full context window.
        chatConfig.arguments << "--kv-unified" << "--kv-unified-per-slot" << QString::number(chatContextSize_);
    }
    // Quantized V cache requires Flash Attention. Keep the CPU fallback at F16.
    const auto cacheType = useCuda && chatKvCacheType_ == "q8_0" ? QString("q8_0") : QString("f16");
    chatConfig.arguments << "--cache-type-k" << cacheType << "--cache-type-v" << cacheType;
    if (cacheType == "q8_0") chatConfig.arguments << "--flash-attn" << "on";
    if (chatKvCacheType_ != "f16" && chatKvCacheType_ != "q8_0")
        emit serverLogMessage("Qwen", QStringLiteral("Неизвестный chatKvCacheType; используется f16. Допустимы f16 и q8_0."));
    emit serverLogMessage("Qwen", QStringLiteral("Настройки памяти: слоёв на GPU %1, KV-кэш %2, окно %3; обработчик изображений: %4")
        .arg(useCuda ? chatGpuLayers_ : QString("0")).arg(cacheType).arg(chatContextSize_)
        .arg(useCuda && projectorOnGpu_ ? QString("GPU") : QString("CPU")));
    emit serverLogMessage("Qwen", QStringLiteral("Распознавание изображений: запросов одновременно %1%2")
        .arg(visionParallelRequests_).arg(visionParallelRequests_ > 1 ? QStringLiteral(" · общий KV-кэш") : QString()));
    if (QFileInfo(projectorPath_).isFile()) {
        chatConfig.arguments << "--mmproj" << projectorPath_ << "--image-max-tokens" << "1024";
        if (!useCuda || !projectorOnGpu_) chatConfig.arguments << "--no-mmproj-offload";
    } else {
        emit serverLogMessage("Qwen", QStringLiteral("mmproj не найден: изображения не будут распознаны. Проверьте models/chatMmproj."));
    }

    LlamaServerConfig embeddingConfig;
    embeddingConfig.executablePath = executable;
    embeddingConfig.healthUrl = QUrl(QString("http://127.0.0.1:%1/health").arg(embeddingPort_));
    embeddingConfig.startupTimeoutMs = startupTimeoutMs_;
    embeddingConfig.arguments = QStringList() << "--model" << embeddingModelPath_
        << "--embedding" << "--alias" << "embeddinggemma" << "--host" << "127.0.0.1"
        << "--port" << QString::number(embeddingPort_) << "--ctx-size" << "4096"
        << "--parallel" << "2" << "--batch-size" << "2048" << "--ubatch-size" << "2048"
        << "--gpu-layers" << gpuLayers;

    chatServer_ = new LlamaServerProcess(chatConfig, this);
    embeddingServer_ = new LlamaServerProcess(embeddingConfig, this);
    auto connectServer = [this](LlamaServerProcess* server, const QString& name) {
        connect(server, &LlamaServerProcess::statusChanged, this, [this, name](const QString& status) {
            const QString text = status + QStringLiteral(" · ") + runtimeBackend_;
            if (name == "Qwen") {
                chatServerStatus_ = text;
            } else {
                embeddingServerStatus_ = text;
            }
            emit serverStatusChanged(name, text);
            updateModelsReady();
        });
        connect(server, &LlamaServerProcess::logMessage, this, [this, name](const QString& text) {
            emit serverLogMessage(name, text);
        });
        connect(server, &LlamaServerProcess::ready, this, &AssistantService::tryStartNextDocument);
        connect(server, &LlamaServerProcess::stopped, this, [this]() {
            updateModelsReady();
            updateServersStopped();
        });
        connect(server, &LlamaServerProcess::errorOccurred, this, [this, name](const QString& message) {
            handleServerFailure(name + QStringLiteral(": ") + message);
        });
    };
    connectServer(chatServer_, "Qwen");
    connectServer(embeddingServer_, "EmbeddingGemma");
    // Both objects and all connections must exist before either start can fail.
    chatServer_->start();
    embeddingServer_->start();
}

void AssistantService::handleServerFailure(const QString& message)
{
    if (shuttingDown_) {
        return;
    }
    embeddingClient_->cancelRequests();
    llmClient_->cancelRequests();
    documentManager_->cancelLoading();
    emit documentProgress(QStringLiteral("Обработка остановлена: сервер недоступен"), false);
    const bool questionPending = importingAttachments_ || state_ == AssistantState::Preparing || state_ == AssistantState::Querying || state_ == AssistantState::Generating;
    importingAttachments_ = false;
    if (state_ == AssistantState::Indexing || state_ == AssistantState::Saving) finishDocumentIndexing(false);
    pendingBatchSize_ = 0;
    pendingQuestion_.clear();
    state_ = AssistantState::Idle;
    busy_ = false;
    emit busyChanged(isBusy());
    updateModelsReady();
    if (questionPending) {
        emit questionFailed(message);
    }
    emit errorOccurred(message);
}

void AssistantService::stopServers()
{
    shuttingDown_ = true;
    imports_->stop();
    storage_->stop();
    documentManager_->cancelLoading();
    if (backendProbe_ && backendProbe_->state() != QProcess::NotRunning) {
        backendProbe_->kill();
    }
    embeddingClient_->cancelRequests();
    llmClient_->cancelRequests();
    if (chatServer_) {
        chatServer_->stop();
    }
    if (embeddingServer_) {
        embeddingServer_->stop();
    }
    if (mcpClient_) {
        mcpClient_->stop();
    }
    updateModelsReady();
    updateServersStopped();
}

void AssistantService::updateServersStopped()
{
    if (!shuttingDown_ || shutdownReported_) {
        return;
    }
    if ((chatServer_ && !chatServer_->isStopped()) ||
        !storage_->isStopped() ||
        (embeddingServer_ && !embeddingServer_->isStopped()) ||
        (backendProbe_ && backendProbe_->state() != QProcess::NotRunning)) {
        return;
    }
    shutdownReported_ = true;
    emit serversStopped();
}

