#include "mcp_client.hpp"

#include <QJsonObject>
#include <QJsonDocument>
#include <QByteArray>
#include <QJsonArray>
#include <QDebug>

MCPClient::MCPClient(QObject* parent) : QObject(parent) {
    pollTimer_ = new QTimer(this);
    pollTimer_->setInterval(2000);

    requestTimeoutTimer_ = new QTimer(this);
    requestTimeoutTimer_->setInterval(5000);
    requestTimeoutTimer_->setSingleShot(true);

    connect(pollTimer_, &QTimer::timeout,
            this, &MCPClient::requestSystemStatus);

    connect(requestTimeoutTimer_, &QTimer::timeout,
            this, [this]() {
        pollTimer_->stop();
        pollingPausedByTimeout_ = true;
        emit errorOccurred("MCP system status request timed out; polling stopped");
    });

    process_ = new QProcess(this);

    connect(process_, &QProcess::readyReadStandardOutput,
            this, [this]() {
        outputBuffer_.append(process_->readAllStandardOutput());

        int newlineIdx;
        while ((newlineIdx = outputBuffer_.indexOf('\n')) != -1) {
            const QByteArray line = outputBuffer_.left(newlineIdx);
            outputBuffer_.remove(0, newlineIdx + 1);

            if (line.trimmed().isEmpty()) {
                continue;
            }

            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);

            if (parseError.error != QJsonParseError::NoError) {
                emit errorOccurred(parseError.errorString());
                continue;
            }

            if (!doc.isObject()) {
                emit errorOccurred("MCP message must be an object");
                continue;
            }

            const QJsonObject message = doc.object();
            qDebug().noquote() << line;

            if (message["id"].toInt() == 1) {
                if (message.contains("error")) {
                    emit errorOccurred("MCP initialization failed");
                    continue;
                }


                if (!message["result"].isObject()) {
                    emit errorOccurred("MCP initialize result is missing");
                    continue;
                }

                const QJsonObject result = message["result"].toObject();
                if (result["protocolVersion"].toString() != "2025-11-25") {
                    emit errorOccurred("Unsupported MCP protocol version");
                    continue;
                }

                sendInitialized();
                qDebug() << "MCP initialized notification sent";
                requestTools();
                qDebug() << "MCP request tools";
            } else if (message["id"].toInt() == 2) {
                if (message.contains("error")) {
                    emit errorOccurred("MCP tools/list request failed");
                    continue;
                }


                if (!message["result"].isObject()) {
                    emit errorOccurred("MCP tools/list result is missing or not an object");
                    continue;
                }

                const QJsonObject result = message["result"].toObject();
                if (!result["tools"].isArray()) {
                    emit errorOccurred("MCP tools/list: tools is missing or not an array");
                    continue;
                }

                requestSystemStatus();
                pollTimer_->start();
                qDebug() << "MCP request system status";
            } else if (message["id"].toInt() == 3) {
                requestTimeoutTimer_->stop();
                systemStatusPending_ = false;
                if (message.contains("error")) {
                    emit errorOccurred("MCP request system status failed");
                    continue;
                }


                if (!message["result"].isObject()) {
                    emit errorOccurred("MCP request system status is missing or not an object");
                    continue;
                }

                const QJsonObject result = message["result"].toObject();
                if (result["isError"].toBool()) {
                    emit errorOccurred("MCP request system status have error");
                    continue;
                }

                if (!result["content"].isArray()) {
                    emit errorOccurred("MCP request system status is missing or not an array");
                    continue;
                }

                const QJsonArray content = result["content"].toArray();

                if (content.isEmpty()) {
                    emit errorOccurred("MCP request system status array is empty");
                    continue;
                }

                if (!content[0].isObject()) {
                    emit errorOccurred("MCP request system status array value is not object");
                    continue;
                }

                const QJsonObject block = content[0].toObject();

                if (!block["type"].isString() || (block["type"].toString() != "text")) {
                    emit errorOccurred("MCP request system status type is not string or not text");
                    continue;
                }

                if (!block["text"].isString()) {
                    emit errorOccurred("MCP request system status text is not string");
                    continue;
                }

                const QString systemText = block["text"].toString();
                qDebug().noquote() << systemText;

                QByteArray bytes = systemText.toUtf8();
                QJsonParseError parseErr;
                const QJsonDocument d = QJsonDocument::fromJson(bytes, &parseErr);

                if (parseErr.error != QJsonParseError::NoError) {
                    emit errorOccurred(parseErr.errorString());
                    continue;
                }

                if (!d.isObject()) {
                    emit errorOccurred("MCP message must be an object");
                    continue;
                }

                const QJsonObject msg = d.object();

                if (msg.contains("error")) {
                    emit errorOccurred("MCP system status failed");
                    continue;
                }


                if (!msg["cpu_percent"].isDouble()) {
                    emit errorOccurred("MCP system status cpu result is missing");
                    continue;
                }

                if (!msg["memory_percent"].isDouble()) {
                    emit errorOccurred("MCP system status memory result is missing");
                    continue;
                }

                double cpu_percent = msg["cpu_percent"].toDouble();
                double memory_percent = msg["memory_percent"].toDouble();
                qDebug() << "CPU:" << cpu_percent << "| RAM:" << memory_percent;

                if (pollingPausedByTimeout_) {
                    pollingPausedByTimeout_ = false;
                    pollTimer_->start();
                    qDebug() << "MCP polling resumed";
                }

                emit systemStatusReady(cpu_percent, memory_percent);
            }
        }
    });

    connect(process_, &QProcess::readyReadStandardError,
            this, [this]() {
        qDebug().noquote() << process_->readAllStandardError();
    });

    connect(process_, &QProcess::errorOccurred,
            this, [this](QProcess::ProcessError) {
        emit errorOccurred(process_->errorString());
    });

    connect(process_, &QProcess::started,
            this, [this]() {
       qDebug() << "MCP server process started";
       sendInitialize();
    });
}

void MCPClient::sendInitialize() {
    QJsonObject clientInfo;
    clientInfo["name"] = "IndustrialAiAssistant";
    clientInfo["version"] = "0.1.0";

    QJsonObject params;
    params["protocolVersion"] = "2025-11-25";
    params["capabilities"] = QJsonObject{};
    params["clientInfo"] = clientInfo;

    QJsonObject message;
    message["jsonrpc"] = "2.0";
    message["id"] = 1;
    message["method"] = "initialize";
    message["params"] = params;

    QByteArray bytes = QJsonDocument(message).toJson(QJsonDocument::Compact);
    bytes.append('\n');

    process_->write(bytes);
}

void MCPClient::sendInitialized() {
    QJsonObject message;
    message["jsonrpc"] = "2.0";
    message["method"] = "notifications/initialized";

    QByteArray bytes = QJsonDocument(message).toJson(QJsonDocument::Compact);
    bytes.append('\n');

    process_->write(bytes);
}

void MCPClient::requestTools() {
    QJsonObject message;
    message["jsonrpc"] = "2.0";
    message["id"] = 2;
    message["method"] = "tools/list";

    QByteArray bytes = QJsonDocument(message).toJson(QJsonDocument::Compact);
    bytes.append('\n');

    process_->write(bytes);
}

void MCPClient::requestSystemStatus() {
    if (systemStatusPending_) {
        return;
    }
    QJsonObject params;
    params["name"] = "get_system_status";
    params["arguments"] = QJsonObject{};

    QJsonObject message;
    message["jsonrpc"] = "2.0";
    message["id"] = 3;
    message["method"] = "tools/call";
    message["params"] = params;

    QByteArray bytes = QJsonDocument(message).toJson(QJsonDocument::Compact);
    bytes.append('\n');

    systemStatusPending_ = true;
    requestTimeoutTimer_->start();

    process_->write(bytes);
}

void MCPClient::start() {
    process_->start(
        "C:/Qt/IndustrialAiAssistant/mcp_server/.venv/Scripts/python.exe",
        QStringList()
                << "-u"
                << "C:/Qt/IndustrialAiAssistant/mcp_server/server.py");
}

void MCPClient::stop() {
    pollTimer_->stop();
    requestTimeoutTimer_->stop();
    pollingPausedByTimeout_ = false;
    process_->closeWriteChannel();
}

MCPClient::~MCPClient() {
    if (process_->state() == QProcess::NotRunning) {
        return;
    }

    stop();

    if (!process_->waitForFinished(1000)) {
        process_->kill();
        process_->waitForFinished(1000);
    }
}
