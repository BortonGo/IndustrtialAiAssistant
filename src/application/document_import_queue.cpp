#include "application/document_import_queue.hpp"
#include "storage/storage_codec.hpp"
#include "rag/document_chunker.h"
#include <QFileInfo>
#include <QUuid>
#include <algorithm>
#include <stdexcept>

DocumentImportQueue::DocumentImportQueue(StorageClient* storage, const DocumentExtractionConfig& config,
                                       const ModelApiConfig& embeddings, QObject* parent)
    : QObject(parent), storage_(storage), reader_(config), embeddings_(embeddings), maxFileSize_(config.maxFileSize) {
    connect(storage_, &StorageClient::ready, this, &DocumentImportQueue::refresh);
    progressTimer_.setSingleShot(true); progressTimer_.setInterval(750);
    connect(&progressTimer_, &QTimer::timeout, this, [this]() {
        if (current_.isEmpty() || stopping_ || phase_ != Phase::Reading) return;
        storage_->request("update_import", {{"id",current_["id"]},{"status","running"},{"progress",progress_}},
                          [this](const QJsonObject&, const QString& error) { if (!error.isEmpty() && !stopping_) emit logMessage(error); });
    });
    connect(&reader_, &DocumentLoader::progressChanged, this, &DocumentImportQueue::progress);
    connect(&reader_, &DocumentLoader::documentReady, this, [this](const Document& document) {
        if (phase_ != Phase::Reading) return;
        if (!stopStatus_.isEmpty()) { finish(stopStatus_); return; }
        acceptDocument(document);
    });
    connect(&reader_, &DocumentLoader::errorOccurred, this, [this](const QString& error) {
        if (phase_ != Phase::Reading) return;
        if (!stopStatus_.isEmpty()) finish(stopStatus_);
        else finish("failed", error);
    });
    connect(&embeddings_, &EmbeddingClient::errorOccurred, this, [this](const QString& error) {
        if (phase_ == Phase::Indexing) finish("failed", error);
    });
    connect(&embeddings_, &EmbeddingClient::embeddingsReady, this, [this](const std::vector<std::vector<double>>& vectors) {
        if (phase_ != Phase::Indexing) return;
        if (vectors.size() != batchSize_) { finish("failed", QStringLiteral("Неверный размер пакета embeddings")); return; }
        QJsonArray batch;
        try {
            VectorStore::InMemoryVectorStore validation;
            for (std::size_t i = 0; i < vectors.size(); ++i) {
                VectorStore::Entry entry{chunks_[entries_.size() + i], vectors[i]};
                validation.add(entry); batch.append(entryToJson(entry));
            }
        } catch (const std::exception& error) { finish("failed", QString::fromUtf8(error.what())); return; }
        phase_ = Phase::Checkpoint;
        const auto text = QStringLiteral("Индексация: %1/%2 фрагментов").arg(entries_.size() + batch.size()).arg(chunks_.size());
        storage_->request("checkpoint_import", {{"id",current_["id"]},{"entries",batch},{"offset",entries_.size()},{"progress",text}},
            [this,batch,text](const QJsonObject&, const QString& error) {
            if (stopping_) return;
            if (!error.isEmpty()) { finish("failed", error); return; }
            for (const auto& entry : batch) entries_.append(entry);
            progress(text); nextBatch();
        });
    });
}

bool DocumentImportQueue::isIdle() const { return phase_ == Phase::Idle; }
QString DocumentImportQueue::activeChatId() const { return current_.value("chatId").toString(); }

void DocumentImportQueue::publish(const QJsonObject& job) {
    if (job.isEmpty()) return;
    ++refreshVersion_; // A previously requested list must not overwrite a newer transition.
    bool found = false;
    for (int i = 0; i < jobs_.size(); ++i) if (jobs_[i].toObject()["id"] == job["id"]) {
        jobs_[i] = job; found = true; break;
    }
    if (!found) jobs_.append(job);
    if (job.value("status").toString() == "complete" || job.value("status").toString() == "cancelled") {
        for (int i = jobs_.size() - 1; i >= 0; --i) if (jobs_[i].toObject()["id"] == job["id"]) jobs_.removeAt(i);
    }
    emit changed();
}

void DocumentImportQueue::refresh() {
    if (stopping_ || !storage_->isReady()) return;
    const auto version = ++refreshVersion_;
    storage_->request("list_imports", {}, [this,version](const QJsonObject& data, const QString& error) {
        if (stopping_) return;
        if (!error.isEmpty()) { emit errorOccurred(error); return; }
        if (version != refreshVersion_) { refresh(); return; }
        jobs_ = data.value("jobs").toArray(); emit changed(); pump();
    });
}

void DocumentImportQueue::enqueue(const QString& chatId, const QString& path) {
    if (stopping_ || chatId.isEmpty()) return;
    storage_->request("enqueue_import", {{"id",QUuid::createUuid().toString().mid(1,36)}, {"chatId",chatId},
                                        {"sourcePath",path},{"maxFileSize",double(maxFileSize_)}},
        [this](const QJsonObject& data, const QString& error) {
        if (stopping_) return;
        if (!error.isEmpty()) { emit errorOccurred(error); return; }
        publish(data.value("job").toObject()); pump();
    });
}

void DocumentImportQueue::setModelsReady(bool ready) {
    modelsReady_ = ready;
    if (!ready && !isIdle()) interrupt("paused");
    pump();
}

void DocumentImportQueue::setForegroundActive(bool active) {
    foreground_ = active;
    if (active && !isIdle()) interrupt("queued");
    if (!active) pump();
}

void DocumentImportQueue::control(const QString& id, const QString& action) {
    if (stopping_ || (action != "paused" && action != "queued" && action != "cancelled")) return;
    if (current_.value("id").toString() == id) {
        if (action != "queued") interrupt(action);
        return;
    }
    storage_->request("update_import", {{"id",id},{"status",action},
        {"progress",action == "queued" ? QStringLiteral("В очереди") : QStringLiteral("Приостановлено")}},
        [this](const QJsonObject& data, const QString& error) {
        if (stopping_) return;
        if (!error.isEmpty()) { emit errorOccurred(error); refresh(); return; }
        publish(data.value("job").toObject()); pump();
    });
}

void DocumentImportQueue::pump() {
    if (!isIdle() || stopping_ || foreground_ || !modelsReady_ || !storage_->isReady()) return;
    for (const auto& value : jobs_) {
        const auto job = value.toObject();
        if (job.value("status").toString() != "queued") continue;
        current_ = job; phase_ = Phase::Claiming; stopStatus_.clear(); progress_ = job.value("progress").toString();
        storage_->request("start_import", {{"id",job["id"]}}, [this](const QJsonObject& data, const QString& error) {
            if (stopping_) return;
            if (!error.isEmpty()) { emit logMessage(error); release(); refresh(); return; }
            current_ = data.value("job").toObject();
            const auto checkpoint = current_.take("checkpoint").toObject(); publish(current_);
            if (current_.value("status").toString() != "running") { release(); return; }
            if (!stopStatus_.isEmpty()) { finish(stopStatus_); return; }
            if (!checkpoint.isEmpty()) {
                try {
                    const auto document = documentFromJson(checkpoint.value("document").toObject());
                    bool imagesExist = true;
                    for (const auto& block : document.blocks)
                        if (block.type == DocumentBlockType::Image && !QFileInfo::exists(block.imagePath)) imagesExist = false;
                    if (imagesExist) { acceptDocument(document, checkpoint.value("entries").toArray(), true); return; }
                } catch (const std::exception& error) { emit logMessage(QString::fromUtf8(error.what())); }
            }
            phase_ = Phase::Reading; progress(QStringLiteral("Чтение документа…"));
            reader_.loadFile(current_.value("sourcePath").toString());
        });
        return;
    }
}

void DocumentImportQueue::acceptDocument(const Document& document, const QJsonArray& entries, bool restored) {
    document_ = document; document_.id = current_.value("documentId").toString();
    entries_ = entries;
    try {
        chunks_ = DocumentChunker::chunkDocument(document_, 500, 100);
        if (chunks_.empty() || entries_.size() > static_cast<int>(chunks_.size())) throw std::runtime_error("Invalid import checkpoint");
        VectorStore::InMemoryVectorStore validation;
        for (int i = 0; i < entries_.size(); ++i) {
            const auto entry = entryFromJson(entries_[i].toObject());
            if (entry.chunk.documentId != document_.id || entry.chunk.text != chunks_[i].text || entry.chunk.startOffset != chunks_[i].startOffset)
                throw std::runtime_error("Import checkpoint does not match document");
            validation.add(entry);
        }
    } catch (const std::exception& error) { finish("failed", QString::fromUtf8(error.what())); return; }
    if (restored) { nextBatch(); return; }
    phase_ = Phase::Checkpoint; progressTimer_.stop();
    storage_->request("checkpoint_import", {{"id",current_["id"]},{"document",documentToJson(document_)},
                                          {"progress",QStringLiteral("Текст извлечён; индексация")}},
        [this](const QJsonObject&, const QString& error) {
        if (stopping_) return;
        if (!error.isEmpty()) { finish("failed", error); return; }
        nextBatch();
    });
}

void DocumentImportQueue::nextBatch() {
    if (!stopStatus_.isEmpty()) { finish(stopStatus_); return; }
    if (entries_.size() == static_cast<int>(chunks_.size())) {
        phase_ = Phase::Saving; progress(QStringLiteral("Сохранение документа…"));
        const auto chatId = activeChatId(), file = QFileInfo(document_.sourcePath).fileName();
        storage_->request("save_document", {{"importId",current_["id"]},{"chatId",chatId},
                          {"document",documentToJson(document_)},{"entries",entries_}},
            [this,chatId,file](const QJsonObject&, const QString& error) {
            if (stopping_) return;
            if (!error.isEmpty()) { finish("failed", error); return; }
            current_["status"] = "complete"; publish(current_);
            emit documentSaved(chatId, file); release();
        });
        return;
    }
    phase_ = Phase::Indexing;
    batchSize_ = std::min<std::size_t>(16, chunks_.size() - entries_.size());
    QStringList texts;
    for (std::size_t i = 0; i < batchSize_; ++i) texts.append(chunks_[entries_.size() + i].text);
    progress(QStringLiteral("Индексация: %1/%2 фрагментов").arg(entries_.size()).arg(chunks_.size()));
    embeddings_.requestEmbeddings(texts);
}

void DocumentImportQueue::progress(const QString& text) {
    if (stopping_ || current_.isEmpty()) return;
    progress_ = text; current_["progress"] = text; publish(current_);
    emit logMessage(QFileInfo(current_.value("sourcePath").toString()).fileName() + ": " + text);
    if (!progressTimer_.isActive() && phase_ == Phase::Reading) progressTimer_.start();
}

void DocumentImportQueue::interrupt(const QString& status) {
    if (isIdle() || stopping_) return;
    // An explicit pause/cancel must survive a later foreground preemption.
    if (stopStatus_.isEmpty() || status == "cancelled" || (status == "paused" && stopStatus_ == "queued")) stopStatus_ = status;
    progressTimer_.stop();
    if (phase_ == Phase::Reading) reader_.cancel();
    else if (phase_ == Phase::Indexing) { embeddings_.cancelRequests(); finish(stopStatus_); }
    // DB transactions/checkpoints finish first; their callbacks honour stopStatus_.
}

void DocumentImportQueue::finish(const QString& status, const QString& error) {
    if (stopping_) return;
    phase_ = Phase::Finishing; progressTimer_.stop();
    const auto text = status == "queued" ? QStringLiteral("Ожидает завершения ответа; прогресс сохранён") :
                      status == "paused" ? QStringLiteral("Приостановлено · ") + progress_ : progress_;
    storage_->request("update_import", {{"id",current_["id"]},{"status",status},{"progress",text},{"error",error}},
        [this,status](const QJsonObject& data, const QString& failure) {
        if (stopping_) return;
        if (failure.isEmpty()) {
            current_ = data.value("job").toObject(); publish(current_);
            if (status == "queued" && (stopStatus_ == "paused" || stopStatus_ == "cancelled")) {
                finish(stopStatus_); return;
            }
        }
        else { modelsReady_ = false; emit errorOccurred(failure); }
        release();
    });
}

void DocumentImportQueue::release() {
    phase_ = Phase::Idle; current_ = {}; document_ = {}; chunks_.clear(); entries_ = {}; stopStatus_.clear();
    progressTimer_.stop(); emit changed(); emit idle();
    QTimer::singleShot(0, this, [this]() { pump(); });
}

void DocumentImportQueue::stop() {
    if (stopping_) return;
    stopping_ = true; progressTimer_.stop(); embeddings_.cancelRequests(); reader_.cancel();
    // A running row is recovered as paused on startup, even after a forced termination.
}
