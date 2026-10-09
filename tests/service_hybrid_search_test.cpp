// Opt-in end-to-end check: configured local models, generated text, private PostgreSQL.
#include "application/assistant_service.hpp"
#include "application/document_import_queue.hpp"
#include "chat/chat_manager.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <QFile>
#include <QTimer>
#include <QElapsedTimer>
#include <QEventLoop>
#include <cstdio>
#include <stdexcept>

static void require(bool ok, const char* error) { if (!ok) throw std::runtime_error(error); }
static void waitFor(const std::function<bool()>& condition, int timeout = 180000) {
    QElapsedTimer time; time.start();
    while (!condition() && time.elapsed() < timeout) {
        QEventLoop loop; QTimer::singleShot(20, &loop, &QEventLoop::quit); loop.exec();
    }
    require(condition(), "Service integration timed out");
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv); app.setApplicationName("HybridSearchTests");
    QDir().mkpath("build-qtcreator/test-artifacts/retrieval");
    QTemporaryDir data("build-qtcreator/test-artifacts/retrieval/service-XXXXXX");
    if (!data.isValid()) return 2;
    qputenv("LOCAL_ASSISTANT_STORAGE_DIR", QFileInfo(data.path()+"/db").absoluteFilePath().toLocal8Bit());
    qputenv("LOCAL_ASSISTANT_STORAGE_PORT", "55437");
    AssistantService service; ChatManager chats(&service);
    QString error, answer, snapshot; bool stopped = false;
    std::vector<VectorStore::SearchResult> hits;
    QObject::connect(&service, &AssistantService::errorOccurred, [&](const QString& text) { error = text; });
    QObject::connect(&service, &AssistantService::questionFailed, [&](const QString& text) { error = text; });
    QObject::connect(&chats, &ChatManager::errorOccurred, [&](const QString& text) { error = text; });
    QObject::connect(&service, &AssistantService::serversStopped, [&]() { stopped = true; });
    QObject::connect(&service, &AssistantService::retrievalReady, [&](const std::vector<VectorStore::SearchResult>& value) { hits = value; });
    QObject::connect(&service, &AssistantService::contextSaved, [&](const QString&, const QString& text) { snapshot = text; });
    QObject::connect(&service, &AssistantService::answerReady, [&](const QString& text) { answer = text; });
    const auto ready = [&]() { return service.modelsReady() && !service.isBusy() && !chats.isBusy() && chats.currentChat(); };
    const auto check = [&]() { require(error.isEmpty(), qPrintable(error)); };
    const auto query = [&](const QString& question) {
        answer.clear(); snapshot.clear(); hits.clear();
        require(chats.sendMessage(question), "Question rejected");
        waitFor([&]() { return !error.isEmpty() || (!answer.isEmpty() && !chats.isBusy() && !service.isBusy()); }); check();
        std::fprintf(stderr, "ANSWER: %s\n", answer.toUtf8().constData());
    };
    int result = 1;
    try {
        const auto path = QFileInfo(data.path()+"/equipment.txt").absoluteFilePath();
        QFile file(path); require(file.open(QIODevice::WriteOnly), "Cannot create test document");
        for (int i=0; i<200; ++i) {
            const auto code = i == 50 ? "UZX-417" : i == 150 ? "UZX-418" : QString("UZX-%1").arg(4000+i);
            const auto pressure = i == 50 ? "17.25" : i == 150 ? "9.75" : "3.0";
            auto paragraph = QStringLiteral("Устройство %1. Рабочее давление %2 бар. ").arg(code, pressure);
            paragraph += QStringLiteral("Техническое описание: насос предназначен для подачи жидкости в контур охлаждения. Проверка параметров проводится при штатном режиме работы. ").repeated(3);
            file.write((paragraph+"\n\n").toUtf8());
        }
        file.close(); service.startServers();
        waitFor([&]() { return ready() || !error.isEmpty(); }); check();
        const auto original = chats.currentChat()->id;
        service.loadDocument(path);
        waitFor([&]() { return !error.isEmpty() || (ready() && service.documentManager()->documentCount() == 1 && service.imports()->isIdle()); }); check();
        query(QStringLiteral("Какое рабочее давление у UZX-417 по документу? Укажи число и единицу измерения."));
        require(!hits.empty() && hits[0].chunk.text.contains("UZX-417"), "Exact hit not leading in service retrieval");
        require(snapshot.contains("UZX-417") && snapshot.contains("17.25"), "Exact evidence omitted from model context");
        require(QString(answer).replace(',', '.').contains("17.25"), "Wrong pressure in generated answer");
        // Switching out and back rebuilds the in-memory lexical index from saved entries.
        const auto empty = chats.createChat(); require(!empty.isEmpty(), "New chat failed");
        waitFor([&]() { return !chats.isBusy() && service.isChatReady(empty); });
        require(service.documentManager()->documentCount() == 0, "Another chat inherited documents");
        require(chats.selectChat(original), "Cannot restore indexed chat");
        waitFor([&]() { return ready() && service.isChatReady(original); });
        query(QStringLiteral("Сравни рабочие давления UZX-417 и UZX-418 из документа. Укажи оба кода и оба значения."));
        require(hits.size() >= 2 && (hits[0].chunk.text + hits[1].chunk.text).contains("UZX-418"), "Second code missing from leading hits");
        require(snapshot.contains("17.25") && snapshot.contains("9.75"), "Comparison context lost a device");
        const auto normalized = QString(answer).replace(',', '.');
        require(normalized.contains("17.25") && normalized.contains("9.75"), "Comparison answer omitted a pressure");
        result = 0; std::puts("Real models: exact-code retrieval, comparison, saved-index restoration and chat isolation passed");
    } catch (const std::exception& failure) { std::fprintf(stderr, "FAIL: %s\n", failure.what()); }
    service.stopServers();
    try { waitFor([&]() { return stopped; }, 30000); } catch (...) { result = 1; }
    return result;
}
