// Manual behavioral check using the real LLM client, a running test model and no database.
// context_relevance_test http://127.0.0.1:8088/v1/ path/to/standalone-plate.png
#include "llm/lmstudio_llm_client.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QJsonDocument>
#include <QDebug>
#include <QFile>
#include <QRegularExpression>
#include <cstdio>
#include <stdexcept>

static void require(bool value, const char* error) { if (!value) throw std::runtime_error(error); }

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() < 3 || app.arguments().size() > 4) return 2;
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString& message) {
        std::fprintf(stderr, "%s\n", message.toUtf8().constData());
    });
    try {
        LMStudioLLMClient client({QUrl(app.arguments()[1]), "local-chat"});
        GenerationRequest request;
        request.sources = {
            {QStringLiteral("Источник: standalone-plate.png. Табличка PUMP-417. Давление 7.5 bar, комната 218."), app.arguments()[2]},
            {QStringLiteral("Источник: manual.txt. Инструкция по стенду: допустимая загрузка CPU до 75%, RAM до 80%."), {}},
            {QStringLiteral("Источник: personnel.txt. Сотрудник Алексей Соколов отвечает за оборудование."), {}}};
        request.systemStatus = QStringLiteral("Источник: MCP get_system_status. Текущая загрузка CPU 3.8%, RAM 63.5%. Снимок получен сейчас.");
        const auto noisy = QStringLiteral("Привет, Алексей! На скане PUMP-417, давление 7.5 bar, комната 218. "
                                          "Дополнительно: CPU 2.9%, RAM 64.2%. Я здесь, чтобы помогать.");
        request.history = {{QStringLiteral("меня зовут алексей"), noisy},
                           {QStringLiteral("как меня зовут?"), QStringLiteral("Ты — Алексей. ") + noisy}};
        auto run = [&](const QString& question, bool document, bool metrics, bool technicalDefinition = false) {
            request.question = question;
            QEventLoop loop; QTimer timer; timer.setSingleShot(true);
            QString answer, snapshot, failure;
            bool done = false;
            auto prepared = QObject::connect(&client, &ILLMClient::contextPrepared, [&](const QString& sources, const QString& budget) {
                snapshot = sources; qInfo().noquote() << budget; client.generatePrepared();
            });
            auto received = QObject::connect(&client, &ILLMClient::answerReady, [&](const QString& text) { answer = text; done = true; loop.quit(); });
            auto failed = QObject::connect(&client, &ILLMClient::errorOccurred, [&](const QString& error) { failure = error; done = true; loop.quit(); });
            QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
            client.prepare(request); timer.start(300000); loop.exec();
            QObject::disconnect(prepared); QObject::disconnect(received); QObject::disconnect(failed);
            qInfo().noquote() << QString::fromUtf8(QJsonDocument(QJsonObject{{"question", question}, {"answer", answer},
                {"snapshot", snapshot}, {"error", failure}}).toJson(QJsonDocument::Compact));
            require(done && failure.isEmpty() && !answer.isEmpty(), "Live generation failed");
            const bool hasDocuments = snapshot.contains(".png") || snapshot.contains(".txt");
            require(hasDocuments == document && snapshot.contains("MCP") == metrics, "Wrong evidence selected");
            if (!document && !metrics) {
                for (const auto& word : {"PUMP", "7.5", "218", "63.5", "3.8"})
                    require(!answer.contains(word, Qt::CaseInsensitive), "Unsolicited reference facts leaked into ordinary conversation");
                if (!technicalDefinition)
                    require(!answer.contains("CPU", Qt::CaseInsensitive) && !answer.contains("RAM", Qt::CaseInsensitive),
                            "Unsolicited computer topic leaked into ordinary conversation");
            }
            return answer;
        };
        auto name = run(QStringLiteral("как еще раз меня зовут?"), false, false);
        require(name.contains(QStringLiteral("Алексей"), Qt::CaseInsensitive), "User name lost");
        require(name.size() < 180, "Simple name answer is verbose");
        auto assistant = run(QStringLiteral("А тебя как зовут?"), false, false);
        const QRegularExpression wrongIdentity(QStringLiteral("(?:меня\\s+зовут|мо[её]\\s+имя|я\\s*[—–:-]?)\\s+Алексей"),
                                              QRegularExpression::CaseInsensitiveOption);
        require(assistant.contains("Local Assistant") && !wrongIdentity.match(assistant).hasMatch(), "Speakers mixed");
        run(QStringLiteral("Спасибо!"), false, false);
        auto pressure = run(QStringLiteral("Какое давление на табличке PUMP-417?"), true, false);
        require(QString(pressure).replace(',', '.').contains("7.5") && !pressure.contains("CPU"), "Image answer failed");
        auto usage = run(QStringLiteral("Какая сейчас загрузка процессора и оперативной памяти?"), false, true);
        require(QString(usage).replace(',', '.').contains("63.5") && !usage.contains("PUMP"), "Fresh MCP answer failed");
        run(QStringLiteral("Какое давление на PUMP-417 и какая сейчас загрузка CPU?"), true, true);
        request.history = {{QStringLiteral("В какой комнате устройство PUMP-417?"), QStringLiteral("В комнате 218, по табличке.")}};
        auto followup = run(QStringLiteral("А давление?"), true, false);
        require(QString(followup).replace(',', '.').contains("7.5"), "Document follow-up lost");
        request.history.clear();
        auto unknown = run(QStringLiteral("Как меня зовут?"), false, false);
        require(!unknown.contains(QStringLiteral("Алексей")) && !unknown.contains("Local Assistant") &&
                (unknown.contains(QStringLiteral("не знаю"), Qt::CaseInsensitive) ||
                 unknown.contains(QStringLiteral("не называл"), Qt::CaseInsensitive) ||
                 unknown.contains(QStringLiteral("не сообщ"), Qt::CaseInsensitive)), "Unknown user's name invented");
        run(QStringLiteral("Что такое оперативная память?"), false, false, true);
        auto manual = run(QStringLiteral("Какая допустимая загрузка CPU по инструкции стенда?"), true, false);
        require(manual.contains("75") && !manual.contains("3.8"), "Manual confused with current measurements");
        request.history = {{QStringLiteral("Меня зовут Лиза"), QStringLiteral("Привет, Лиза! Сейчас система работает стабильно: CPU — 5.8%, RAM — 64.1% (последнее обновление — 0 секунд назад).")},
                           {QStringLiteral("как меня зовут?"), QStringLiteral("Ты — Лиза. CPU — 5.8%, RAM — 64.1%.")}};
        const auto lisa = run(QStringLiteral("как меня зовут?"), false, false);
        require(lisa.contains(QStringLiteral("Лиза"), Qt::CaseInsensitive) && !lisa.contains("64.1"), "Lisa screenshot regression");
        run(QStringLiteral("Не показывай состояние системы. Как меня зовут?"), false, false);
        request.history = {{QStringLiteral("Какая сейчас загрузка процессора?"), QStringLiteral("CPU — 5.8%.")}};
        run(QStringLiteral("А память?"), false, true);
        if (app.arguments().size() == 4) {
            QFile fixture(app.arguments()[3]); require(fixture.open(QIODevice::ReadOnly), "Cannot read observed conversation");
            const auto stored = QJsonDocument::fromJson(fixture.readAll()).object();
            request.history.clear();
            for (const auto& pair : stored.value("history").toArray())
                request.history.push_back({pair.toObject().value("question").toString(), pair.toObject().value("answer").toString()});
            run(stored.value("question").toString(), false, false);
        }
        qInfo() << "Relevance, opt-in monitoring, noisy history and observed conversation checks passed";
        return 0;
    } catch (const std::exception& error) { qCritical() << error.what(); return 1; }
}
