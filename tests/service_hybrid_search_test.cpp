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
#include <QUuid>
#include <QDebug>
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
    qInstallMessageHandler([](QtMsgType type,const QMessageLogContext&,const QString& text){
        if(type==QtWarningMsg || type==QtCriticalMsg || type==QtFatalMsg || text.startsWith("Conversation plan:") || text.startsWith("Context selection:"))
            std::fprintf(stderr,"%s\n",text.toUtf8().constData());
    });
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
    QObject::connect(&service, &AssistantService::serverLogMessage, [&](const QString& name,const QString& text) {
        if(name==QStringLiteral("Память") || name==QStringLiteral("Контекст") || name==QStringLiteral("Поиск"))
            std::fprintf(stderr,"%s: %s\n",name.toUtf8().constData(),text.toUtf8().constData());
    });
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
        query(QStringLiteral("А повтори его рабочее давление?"));
        require(!hits.empty() && hits[0].chunk.text.contains("UZX-417"), "Follow-up did not resolve before retrieval");
        require(snapshot.contains("17.25") && QString(answer).replace(',', '.').contains("17.25"), "Follow-up lost grounded pressure");
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
        const auto storageRequest = [&](const QString& method,const QJsonObject& params) {
            bool done=false; QJsonObject result;
            service.storage()->request(method,params,[&](const QJsonObject& value,const QString& failure){result=value;error=failure;done=true;});
            waitFor([&](){return done;});check();return result;
        };
        for(int i=0;i<12;++i) {
            const auto id=QUuid::createUuid().toString().mid(1,36);
            const auto text=i==0 ? QStringLiteral("Меня зовут Алексей.") : QStringLiteral("Спасибо за объяснение пункта %1.").arg(i);
            storageRequest("save_question",{{"id",id},{"chatId",original},{"text",text},{"title","Memory test"}});
            storageRequest("finish_question",{{"questionId",id},{"id",QUuid::createUuid().toString().mid(1,36)},{"text",QStringLiteral("Принято.")}});
        }
        query(QStringLiteral("Как меня зовут?"));
        require(answer.contains(QStringLiteral("Алексей"),Qt::CaseInsensitive) && !answer.contains("17.25"),"Saved memory lost user's name or leaked document facts");
        const auto documentId=service.documentManager()->documentAt(0)->id;
        const auto libraryId=storageRequest("publish_library_document",{{"chatId",original},{"documentId",documentId}}).value("id").toString();
        require(chats.selectChat(empty),"Cannot switch to library target chat");
        waitFor([&](){return ready() && service.isChatReady(empty);});
        bool attached=false;
        service.attachLibraryDocuments(empty,{libraryId},[&](const QString& failure){error=failure;attached=true;});
        waitFor([&](){return attached && ready();}); check();
        require(service.documentManager()->documentCount()==1 && service.documentManager()->documentAt(0)->id!=documentId,
                "Library attachment did not create an isolated chat document");
        query(QStringLiteral("Какое рабочее давление у UZX-417 по документу? Укажи число и единицу измерения."));
        require(!hits.empty() && hits[0].chunk.documentId!=documentId && QString(answer).replace(',', '.').contains("17.25"),
                "Cloned library index lost its evidence or used another chat's IDs");
        query(QStringLiteral("Как меня зовут?"));
        require(!answer.contains(QStringLiteral("Алексей"),Qt::CaseInsensitive),"Library transfer copied conversation memory");
        result = 0; std::puts("Real models: exact-code retrieval, comparison, saved-index restoration and chat isolation passed");
    } catch (const std::exception& failure) { std::fprintf(stderr, "FAIL: %s\n", failure.what()); }
    service.stopServers();
    try { waitFor([&]() { return stopped; }, 30000); } catch (...) { result = 1; }
    return result;
}
