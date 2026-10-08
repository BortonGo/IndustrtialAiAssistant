#include "io/document_extractor.hpp"
#include "io/document_loader.hpp"
#include "rag/document_chunker.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>
#include <stdexcept>
#include <cstdio>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

static void checkWorkerLifecycle(const QDir& repo) {
    QTemporaryDir temp;
    const auto script = QDir(temp.path()).filePath("worker.py");
    QFile file(script);
    if (!file.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot write worker fixture");
    file.write("import json,sys,time,subprocess\n"
               "if sys.argv[1]=='success':\n"
               " print(json.dumps({'text':'ok','blocks':[{'type':'text','text':'ok','pageNumber':0}],'warnings':[]}))\n"
               "else:\n"
               " child=subprocess.Popen([sys.executable,'-c','import time; time.sleep(60)'])\n"
               " print('@@DOCUMENT@@'+json.dumps({'message':'child:'+str(child.pid)}),file=sys.stderr,flush=True)\n"
               " time.sleep(60)\n");
    file.close();
    for (bool timeout : {false, true}) {
        DocumentExtractionConfig config;
        config.pythonPath = repo.filePath("document_tools/.venv/Scripts/python.exe");
        config.scriptPath = script;
        config.inactivityTimeoutMs = timeout ? 1000 : 5000;
        DocumentExtractor extractor(config);
        QEventLoop loop;
        int errors = 0, childId = 0;
        bool success = false;
        QObject::connect(&extractor, &DocumentExtractor::progressChanged, [&](const QString& message) {
            childId = message.mid(6).toInt();
            if (!timeout) extractor.cancel();
        });
        QObject::connect(&extractor, &DocumentExtractor::errorOccurred, [&](const QString& message) {
            ++errors; qInfo().noquote() << message; loop.quit();
        });
        QObject::connect(&extractor, &DocumentExtractor::contentReady, [&](const Document&) { success = true; loop.quit(); });
        QTimer watchdog;
        watchdog.setSingleShot(true);
        QObject::connect(&watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);
        watchdog.start(10000);
        extractor.extract("wait");
        loop.exec();
        if (errors != 1 || !childId) throw std::runtime_error("Worker cancellation/timeout failed");
#ifdef Q_OS_WIN
        HANDLE child = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(childId));
        if (child) {
            const auto state = WaitForSingleObject(child, 1000);
            CloseHandle(child);
            if (state != WAIT_OBJECT_0) throw std::runtime_error("Worker left a child running");
        }
#endif
        extractor.extract("success");
        loop.exec();
        if (!success || errors != 1) throw std::runtime_error("Worker did not recover after cancellation/timeout");
    }
    qInfo() << "Cancellation, timeout, child cleanup and recovery checks passed";
}

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString& message) {
        const auto bytes = message.toUtf8();
        std::fprintf(stderr, "%s\n", bytes.constData());
    });
    QCoreApplication::setApplicationName("LocalAssistantDocumentTests");
    try {
        QTemporaryDir temp;
        QFile file(QDir(temp.path()).filePath("image.png"));
        check(file.open(QIODevice::WriteOnly), "create fixture");
        file.write("fixture"); file.close();
        QJsonArray blocks;
        blocks.append(QJsonObject{{"type", "text"}, {"text", "Before"}, {"pageNumber", 1}});
        blocks.append(QJsonObject{{"type", "image"}, {"text", "PUMP-417 pressure 7.5 bar"},
                                  {"pageNumber", 2}, {"tableIndex", 0}, {"tableRow", 1},
                                  {"tableColumn", 2}, {"imagePath", file.fileName()}});
        const QString text = QStringLiteral("Before\n[Описание изображения моделью; возможны ошибки]\nPUMP-417 pressure 7.5 bar");
        QJsonObject root{{"text", text}, {"blocks", blocks}, {"warnings", QJsonArray()}};
        Document document;
        QString error;
        check(parseDocumentContent(QJsonDocument(root).toJson(), document, error), qPrintable(error));
        document.id = "fixture.pdf";
        auto chunks = DocumentChunker::chunkDocument(document, 55, 10);
        check(chunks.size() > 2, "image text must span chunks");
        for (const auto& chunk : chunks) {
            check(chunk.text == text.mid(chunk.startOffset, chunk.text.size()), "chunk offset mismatch");
            if (chunk.blockIndex == 1) {
                check(chunk.pageNumber == 2 && chunk.tableRow == 1 && chunk.tableColumn == 2, "lost image provenance");
                check(chunk.imagePath == file.fileName(), "lost image reference");
            }
        }
        auto bad = root;
        bad["text"] = "mismatch";
        check(!parseDocumentContent(QJsonDocument(bad).toJson(), document, error), "must reject mismatched text");
        auto image = blocks[1].toObject(); image["pageNumber"] = 1.5;
        blocks[1] = image; bad = root; bad["blocks"] = blocks;
        check(!parseDocumentContent(QJsonDocument(bad).toJson(), document, error), "must reject fractional page");
        file.remove();
        check(!parseDocumentContent(QJsonDocument(root).toJson(), document, error), "must reject missing image");
        Document plain; plain.id = "plain"; plain.text = "abcdefghijklmnopqrst";
        check(DocumentChunker::chunkDocument(plain, 8, 2).size() == 3, "TXT chunk regression");
        Document paragraphs;
        for (const auto& value : {"first", "second", "third"}) {
            DocumentBlock block; block.text = value; paragraphs.blocks.push_back(block);
        }
        paragraphs.text = "first\nsecond\nthird";
        auto combined = DocumentChunker::chunkDocument(paragraphs, 12, 2);
        check(combined.size() == 2 && combined[0].text == "first\nsecond", "short paragraphs should share chunks");
        check(combined[1].blockIndex == 1 && combined[1].startOffset == 10, "combined paragraph provenance");
        qInfo() << "JSON schema, chunk text, offsets, image provenance and TXT checks passed";

        // Optional integration check against the installed Python/Poppler and local model.
        if (app.arguments().size() >= 3) {
            QDir repo(app.arguments()[1]);
            if (app.arguments()[2] == "--lifecycle") {
                checkWorkerLifecycle(repo);
                return 0;
            }
            DocumentExtractionConfig config;
            config.pythonPath = repo.filePath("document_tools/.venv/Scripts/python.exe");
            config.scriptPath = repo.filePath("document_tools/read_document.py");
            config.popplerDirectory = repo.filePath("tools/poppler-26.09.0/Library/bin");
            if (app.arguments().size() > 3) config.visionUrl = app.arguments()[3];
            config.visionProfile = "test-Qwen3VL8B";
            DocumentLoader loader(config);
            QEventLoop loop;
            bool success = false, completed = false;
            QObject::connect(&loader, &DocumentLoader::progressChanged, [](const QString& text) { qInfo().noquote() << text; });
            QObject::connect(&loader, &DocumentLoader::errorOccurred, [&](const QString& text) {
                completed = true; qWarning().noquote() << text; loop.quit();
            });
            QObject::connect(&loader, &DocumentLoader::documentReady, [&](const Document& result) {
                completed = true;
                const auto path = QFileInfo(app.arguments()[2]).canonicalFilePath();
                const auto chunks = DocumentChunker::chunkDocument(result, 500, 100);
                success = !chunks.empty() && result.id == path && result.sourcePath == path;
                for (const auto& chunk : chunks) {
                    success = success && chunk.documentId == path;
                    if (chunk.blockIndex >= 0 && static_cast<std::size_t>(chunk.blockIndex) < result.blocks.size()) {
                        const auto& block = result.blocks[chunk.blockIndex];
                        if (block.type == DocumentBlockType::Image)
                            success = success && chunk.imagePath == block.imagePath;
                    }
                }
                qInfo() << "Worker completed; blocks:" << result.blocks.size() << "warnings:" << result.warnings;
                loop.quit();
            });
            QTimer::singleShot(360000, &loop, &QEventLoop::quit);
            loader.loadFile(app.arguments()[2]);
            if (!completed) loop.exec();
            check(success, "Qt worker integration failed");
        }
    } catch (const std::exception& error) {
        qCritical() << error.what();
        return 1;
    }
    return 0;
}
