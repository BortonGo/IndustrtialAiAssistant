// Real Qt worker/storage, deterministic local embedding server, isolated PostgreSQL.
#include "application/document_import_queue.hpp"
#include "storage/storage_client.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTcpServer>
#include <QTcpSocket>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QUuid>
#include <QDebug>
#include <memory>
#include <stdexcept>

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void waitFor(const std::function<bool()>& condition) {
    QElapsedTimer deadline; deadline.start();
    while (!condition() && deadline.elapsed() < 40000) {
        QEventLoop loop; QTimer::singleShot(10, &loop, &QEventLoop::quit); loop.exec();
    }
    require(condition(), "Timed out waiting for import/storage");
}
QJsonObject request(StorageClient& storage, const QString& method, const QJsonObject& params) {
    bool done = false; QString failure; QJsonObject result;
    storage.request(method, params, [&](const QJsonObject& data, const QString& error) { result = data; failure = error; done = true; });
    waitFor([&]() { return done; }); require(failure.isEmpty(), qPrintable(failure)); return result;
}
void write(const QString& path, const QByteArray& text) { QFile f(path); require(f.open(QIODevice::WriteOnly), "File open"); f.write(text); }
QString uuid() { return QUuid::createUuid().toString().mid(1,36); }
QJsonObject job(const DocumentImportQueue& queue, const QString& path) {
    for (const auto& value : queue.jobs()) if (QDir::fromNativeSeparators(value.toObject().value("sourcePath").toString()) == path) return value.toObject();
    return {};
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("ImportQueueTests");
    const QDir root(QDir::current());
    QDir().mkpath("build-import-queue");
    QTemporaryDir data(root.filePath("build-import-queue/qt-XXXXXX"));
    if (!data.isValid()) return 2;
    qputenv("LOCAL_ASSISTANT_STORAGE_DIR", (data.path() + "/db").toLocal8Bit());
    qputenv("LOCAL_ASSISTANT_STORAGE_PORT", "55435");
    write(data.path()+"/model.gguf", "fixture model");
    StorageClient storage(root.path(), data.path()+"/model.gguf");
    try {
        QTcpServer server; require(server.listen(QHostAddress::LocalHost, 0), "Local server bind");
        int embedded = 0, requests = 0;
        QObject::connect(&server, &QTcpServer::newConnection, [&]() {
            auto* socket = server.nextPendingConnection();
            auto buffer = std::make_shared<QByteArray>();
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [&,socket,buffer]() {
                buffer->append(socket->readAll());
                const int header = buffer->indexOf("\r\n\r\n"); if (header < 0) return;
                int size = 0;
                for (const auto& line : buffer->left(header).split('\n')) if (line.toLower().startsWith("content-length:")) size = line.mid(15).trimmed().toInt();
                if (buffer->size() < header + 4 + size || socket->property("handled").toBool()) return;
                socket->setProperty("handled", true);
                const auto input = QJsonDocument::fromJson(buffer->mid(header+4, size)).object().value("input").toArray();
                embedded += input.size(); ++requests;
                QJsonArray vector; vector.append(1.0); for (int i=1; i<768; ++i) vector.append(0.0);
                QJsonArray values; for (int i=0; i<input.size(); ++i) values.append(QJsonObject{{"index",i},{"embedding",vector}});
                const auto body = QJsonDocument(QJsonObject{{"data",values}}).toJson(QJsonDocument::Compact);
                QTimer::singleShot(200, socket, [socket,body]() {
                    if (socket->state() != QAbstractSocket::ConnectedState) return;
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            });
        });
        const auto worker = data.path()+"/worker.py";
        write(worker, "import json,sys,time\nprint('@@DOCUMENT@@'+json.dumps({'message':'Reading fixture'}),file=sys.stderr,flush=True)\ntime.sleep(0.6)\ntext='PUMP-417 pressure 7.5 bar. '*450\nprint(json.dumps({'text':text,'blocks':[{'type':'text','text':text,'pageNumber':1}],'warnings':[]}))\n");
        DocumentExtractionConfig config;
        config.pythonPath = root.filePath("document_tools/.venv/Scripts/python.exe"); config.scriptPath = worker;
        ModelApiConfig model{QUrl(QString("http://127.0.0.1:%1/v1/").arg(server.serverPort())), "fixture"};
        auto queue = std::unique_ptr<DocumentImportQueue>(new DocumentImportQueue(&storage, config, model));
        QString failure;
        QObject::connect(queue.get(), &DocumentImportQueue::errorOccurred, [&](const QString& error) { failure = error; });
        storage.start(); waitFor([&]() { return storage.isReady(); });
        const auto a = uuid(), b = uuid();
        request(storage, "create_chat", {{"id",a},{"title","A"}});
        request(storage, "create_chat", {{"id",b},{"title","B"}});
        const auto first = data.path()+"/first.docx", second = data.path()+"/second.docx";
        write(first, "first"); write(second, "second");
        queue->enqueue(a, first); queue->enqueue(b, second); queue->enqueue(a, first);
        waitFor([&]() { return queue->jobs().size() == 2; });
        const auto firstId = job(*queue, first).value("id").toString(), secondId = job(*queue, second).value("id").toString();
        queue->control(secondId, "paused");
        waitFor([&]() { return job(*queue, second).value("status").toString() == "paused"; });
        queue->setModelsReady(true);
        waitFor([&]() { return job(*queue, first).value("progress").toString() == "Reading fixture"; });
        queue->setForegroundActive(true);
        waitFor([&]() { return queue->isIdle() && job(*queue, first).value("status").toString() == "queued"; });
        require(embedded == 0, "Preemption did not stop extraction");
        // Other chats remain usable while a suspended background import is queued.
        const auto question = uuid();
        request(storage, "save_question", {{"id",question},{"chatId",b},{"text","Foreground question"},{"title","B"}});
        request(storage, "finish_question", {{"questionId",question},{"id",uuid()},{"text","Foreground answer"}});
        bool pausedAfterBatch = false;
        QObject::connect(queue.get(), &DocumentImportQueue::changed, [&]() {
            if (!pausedAfterBatch && job(*queue, first).value("progress").toString().startsWith(QStringLiteral("Индексация: 16/"))) {
                pausedAfterBatch = true; queue->control(firstId, "paused");
            }
        });
        queue->setForegroundActive(false);
        waitFor([&]() { return pausedAfterBatch && queue->isIdle(); });
        require(job(*queue, first).value("status").toString() == "paused", "Explicit pause lost");
        const int beforeResume = embedded;
        require(beforeResume == 16, "Completed batch was not checkpointed before pause");
        queue->stop(); queue.reset();
        storage.stop(); waitFor([&]() { return storage.isStopped(); });
        queue.reset(new DocumentImportQueue(&storage, config, model));
        storage.start(); waitFor([&]() { return storage.isReady() && queue->jobs().size() == 2; });
        queue->setModelsReady(true);
        require(queue->isIdle(), "Paused import restarted without resume");
        // The native worker must not run again after a saved document checkpoint.
        write(worker, "raise RuntimeError('Extraction checkpoint was ignored')\n");
        queue->control(firstId, "queued");
        waitFor([&]() { return job(*queue, first).isEmpty() && queue->isIdle(); });
        const auto saved = request(storage, "load_chat", {{"chatId",a},{"remember",false}});
        require(saved.value("documents").toArray().size() == 1, "Document did not publish");
        require(saved.value("entries").toArray().size() == embedded, "Resume repeated embeddings or lost chunks");
        require(requests == 2, "Unexpected embedding batches after resume");
        require(request(storage, "load_chat", {{"chatId",b},{"remember",false}}).value("documents").toArray().isEmpty(), "Cross-chat import leak");
        write(second, "changed source");
        queue->control(secondId, "queued");
        waitFor([&]() { return job(*queue, second).value("status").toString() == "failed" && queue->isIdle(); });
        queue->control(secondId, "cancelled"); waitFor([&]() { return queue->jobs().isEmpty(); });
        require(QFileInfo::exists(second), "Cancel deleted the original source");
        require(failure.isEmpty(), qPrintable(failure));
        queue->stop(); queue.reset();
        storage.stop(); waitFor([&]() { return storage.isStopped(); });
        qInfo() << "Import queue: preemption, durable pause, restart, checkpoint resume, deduplication, chat isolation, changed file and cancel passed";
        return 0;
    } catch (const std::exception& error) {
        qCritical() << error.what(); storage.stop(); waitFor([&]() { return storage.isStopped(); }); return 1;
    }
}
