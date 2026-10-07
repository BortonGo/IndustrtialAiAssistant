#include "assistant_service.hpp"
#include "document_chunker.h"
#include "embedding_client.hpp"
#include "lmstudio_llm_client.hpp"
#include "mcp_client.hpp"
#include "llama_server_process.hpp"
#include "runtime_paths.hpp"

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
    constexpr qint64 maxFileSize = 1024 * 1024;
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

    // DocumentManager
    const QString pdfExtractorPath =
        root.filePath("tools/poppler-26.09.0/Library/bin/pdftotext.exe");
    const QString docxPythonPath =
        root.filePath("document_tools/.venv/Scripts/python.exe");
    const QString docxScriptPath =
        root.filePath("document_tools/read_docx.py");

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
    const ModelApiConfig embeddingConfig {
        QUrl(QString("http://127.0.0.1:%1/v1/").arg(embeddingPort_)),
        QString("embeddinggemma")
    };

    embeddingClient_ = new EmbeddingClient(embeddingConfig, this);

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

            emit busyChanged(false);
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

            emit busyChanged(false);
            qDebug() << "Indexing finished";
            tryStartNextDocument();
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

bool AssistantService::askQuestion(const QString &question) {
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
    if (!modelsReady()) {
        emit errorOccurred(QStringLiteral("Дождитесь готовности серверов моделей"));
        return;
    }
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
    emit busyChanged(true);
    requestNextEmbeddingBatch();
    return true;
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
    }
}

void AssistantService::requestNextEmbeddingBatch() {
    if (!modelsReady() || state_ != AssistantState::Indexing ||
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


AssistantService::~AssistantService()
{
    shuttingDown_ = true;
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
    return !shuttingDown_ && chatServer_ && embeddingServer_ &&
           chatServer_->isReady() && embeddingServer_->isReady();
}

bool AssistantService::isBusy() const
{
    return busy_ || state_ != AssistantState::Idle;
}

QString AssistantService::chatServerStatus() const { return chatServerStatus_; }
QString AssistantService::embeddingServerStatus() const { return embeddingServerStatus_; }

void AssistantService::updateModelsReady()
{
    const bool ready = modelsReady();
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
        << "--ctx-size" << "4096" << "--parallel" << "1" << "--gpu-layers" << gpuLayers
        << "--jinja" << "--reasoning" << "off";

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
    const bool questionPending = state_ == AssistantState::Querying || state_ == AssistantState::Generating;
    if (state_ == AssistantState::Indexing) {
        documentManager_->setDocumentStatus(indexingDocumentId_, DocumentStatus::Error);
        vectorStore_.removeDocument(indexingDocumentId_);
        indexingDocumentId_.clear();
    }
    pendingBatchSize_ = 0;
    pendingQuestion_.clear();
    state_ = AssistantState::Idle;
    busy_ = false;
    emit busyChanged(false);
    updateModelsReady();
    if (questionPending) {
        emit questionFailed(message);
    }
    emit errorOccurred(message);
}

void AssistantService::stopServers()
{
    shuttingDown_ = true;
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
        (embeddingServer_ && !embeddingServer_->isStopped()) ||
        (backendProbe_ && backendProbe_->state() != QProcess::NotRunning)) {
        return;
    }
    shutdownReported_ = true;
    emit serversStopped();
}

