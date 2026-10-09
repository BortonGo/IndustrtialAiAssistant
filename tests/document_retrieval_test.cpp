// Manual test: actual embeddings/retrieval/LLM using an exported, read-only fixture.
// Args: chat /v1/ URL, embedding /v1/ URL, fixture.json. Never writes to the chat DB.
#include "rag/document_context.hpp"
#include "rag/embedding_client.hpp"
#include "storage/storage_codec.hpp"
#include "llm/lmstudio_llm_client.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QSettings>
#include <QDebug>
#include <cstdio>
#include <stdexcept>

static void require(bool value, const char* error) { if (!value) throw std::runtime_error(error); }

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString& text) { std::fprintf(stderr,"%s\n",text.toUtf8().constData()); });
    if (app.arguments().size() != 4) return 2;
    try {
        QFile fixture(app.arguments()[3]); require(fixture.open(QIODevice::ReadOnly), "Cannot open fixture");
        const auto data = QJsonDocument::fromJson(fixture.readAll()).object();
        std::vector<Document> ownedDocuments;
        for (const auto& value : data.value("documents").toArray()) ownedDocuments.push_back(documentFromJson(value.toObject()));
        std::vector<const Document*> documents;
        for (const auto& document : ownedDocuments) documents.push_back(&document);
        VectorStore::InMemoryVectorStore index;
        for (const auto& value : data.value("entries").toArray()) index.add(entryFromJson(value.toObject()));
        const auto question = data.value("question").toString();
        const auto scope = DocumentContext::namedDocuments(question, documents);
        require(scope.size() == 1, "Expected exactly one explicitly named document");

        EmbeddingClient embedding({QUrl(app.arguments()[2]),"embeddinggemma"});
        std::vector<double> vector; QString error; bool done = false;
        QEventLoop loop; QTimer timeout; timeout.setSingleShot(true);
        QObject::connect(&timeout,&QTimer::timeout,&loop,&QEventLoop::quit);
        QObject::connect(&embedding,&EmbeddingClient::embeddingReady,[&](const std::vector<double>& value) { vector=value; done=true; loop.quit(); });
        QObject::connect(&embedding,&EmbeddingClient::errorOccurred,[&](const QString& failure) { error=failure; done=true; loop.quit(); });
        embedding.requestEmbedding(question); timeout.start(60000); loop.exec();
        require(done && error.isEmpty() && !vector.empty(), qPrintable("Embedding failed: " + error));
        const auto hits = index.searchHybrid(question,vector,12,scope);
        GenerationRequest request; request.question = question;
        request.sources = DocumentContext::collect(question,documents,hits,scope);
        QSettings settings("local-assistant.ini",QSettings::IniFormat);
        request.contextSize = settings.value("models/chatContextSize",8192).toInt();
        request.maxAnswerTokens = settings.value("models/maxAnswerTokens",4096).toInt();
        require(request.sources.size() == 1 && request.sources[0].required, "Small named document not provided as a complete source");
        qInfo().noquote() << "Retrieved" << hits.size() << "hits; complete document source:" << request.sources[0].text.size() << "characters";

        LMStudioLLMClient llm({QUrl(app.arguments()[1]),"local-chat"});
        QString answer,snapshot; done=false;
        QObject::connect(&llm,&ILLMClient::contextPrepared,[&](const QString& sources,const QString& budget) {
            snapshot=sources; qInfo().noquote() << budget; llm.generatePrepared();
        });
        QObject::connect(&llm,&ILLMClient::answerReady,[&](const QString& text) { answer=text; done=true; loop.quit(); });
        QObject::connect(&llm,&ILLMClient::errorOccurred,[&](const QString& failure) { error=failure; done=true; loop.quit(); });
        llm.prepare(request); timeout.start(1800000); loop.exec();
        require(done && error.isEmpty(), qPrintable("Generation failed: " + error));
        const auto folder = QFileInfo(fixture.fileName()).absoluteDir();
        QFile savedAnswer(folder.filePath("protocol-answer.md")); require(savedAnswer.open(QIODevice::WriteOnly), "Cannot save answer");
        savedAnswer.write(answer.toUtf8()); savedAnswer.close();
        QFile savedSources(folder.filePath("protocol-sources.txt")); require(savedSources.open(QIODevice::WriteOnly), "Cannot save sources");
        savedSources.write(snapshot.toUtf8()); savedSources.close();
        require(snapshot.contains(request.sources[0].text), "Prepared context lost part of the named document");
        require(answer.size() > 1500, "Detailed protocol question received only a summary");
        require(!answer.contains(QStringLiteral("Ответ не завершён")), "Answer hit the generation limit");
        for (const auto& section : data.value("expectedSections").toArray())
            require(answer.contains(section.toString()), qPrintable("Missing section " + section.toString()));
        for (const auto& phrase : data.value("forbiddenPhrases").toArray())
            require(!answer.contains(phrase.toString(), Qt::CaseInsensitive), qPrintable("Unsupported statement: " + phrase.toString()));
        qInfo().noquote() << "Complete protocol document delivered; structured answer saved:" << savedAnswer.fileName() << "; characters:" << answer.size();
        return 0;
    } catch (const std::exception& error) { qCritical() << error.what(); return 1; }
}
