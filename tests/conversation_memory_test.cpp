// Opt-in behavioral check against the actual model; never opens a database.
#include "llm/lmstudio_llm_client.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QDebug>
#include <cstdio>
#include <stdexcept>
static void check(bool ok,const char* error) { if(!ok)throw std::runtime_error(error); }
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv); if(app.arguments().size()!=2)return 2;
    qInstallMessageHandler([](QtMsgType,const QMessageLogContext&,const QString& text){std::fprintf(stderr,"%s\n",text.toUtf8().constData());});
    try {
        LMStudioLLMClient client({QUrl(app.arguments()[1]),"local-chat"});
        QJsonArray quotes{QJsonObject{{"questionId","q1"},{"text",QStringLiteral("Меня зовут Алексей.")}},
                          QJsonObject{{"questionId","q2"},{"text",QStringLiteral("Проект должен работать без интернета.")}}};
        QJsonArray pairs{QJsonObject{{"question",QStringLiteral("Где расположен насос PUMP-417?")},{"answer",QStringLiteral("В комнате 218. CPU 55%.")}}};
        const auto plan=[&](const QString& question, bool followup = true) {
            QEventLoop loop; bool done=false; QString error; QJsonObject result;
            client.planConversation(question,{{"compactThrough",1},{"candidates",quotes},{"pairs",pairs}},8192,followup,
                [&](const QJsonObject& value,const QString& failure){result=value;error=failure;done=true;loop.quit();});
            QTimer::singleShot(90000,&loop,&QEventLoop::quit);if(!done)loop.exec();
            check(done && error.isEmpty(),qPrintable(error));qInfo().noquote()<<"Plan:"<<question<<result;return result;
        };
        auto pressure=plan(QStringLiteral("А какое у него давление?"));
        check(pressure["anchor"].toInt(-1)==0 && pressure["query"].toString().contains("PUMP-417"),"Follow-up lost pump identity");
        check(pressure["memory"].toArray().contains(0),"User's name not retained in memory");
        check(plan(QStringLiteral("А как сварить гречку?"))["anchor"].toInt(-1)==-1,"New topic retained document scope");
        pairs.append(QJsonObject{{"question",QStringLiteral("Теперь речь про клапан VALVE-902.")},{"answer",QStringLiteral("Обсуждаем VALVE-902.")}});
        auto valve=plan(QStringLiteral("А какое у него давление?"));
        check(valve["anchor"].toInt(-1)==1 && valve["query"].toString().contains("VALVE-902") && !valve["query"].toString().contains("PUMP-417"),"Topic correction ignored");
        const auto savedQuotes = quotes;
        quotes = {QJsonObject{{"text",QStringLiteral("Какое рабочее давление у UZX-417 по документу?")}},
                  QJsonObject{{"text",QStringLiteral("Укажи число и единицу измерения.")}},
                  QJsonObject{{"text",QStringLiteral("А повтори его рабочее давление?")}},
                  QJsonObject{{"text",QStringLiteral("Сравни рабочие давления UZX-417 и UZX-418 из документа.")}},
                  QJsonObject{{"text",QStringLiteral("Укажи оба кода и оба значения.")}},
                  QJsonObject{{"text",QStringLiteral("Меня зовут Алексей.")}}};
        pairs = {};
        for (int i=0;i<12;++i) pairs.append(QJsonObject{{"question",QStringLiteral("Спасибо за объяснение пункта %1.").arg(i)},{"answer",QStringLiteral("Принято.")}});
        check(plan(QStringLiteral("Как меня зовут?"),false)["memory"].toArray().contains(5),"Name among old document questions was discarded");
        quotes = savedQuotes;
        GenerationRequest request;request.contextSize=8192;request.maxAnswerTokens=256;request.memory=quotes;
        const auto answer=[&](const QString& question) {
            QEventLoop loop;bool done=false;QString result,error;
            auto ready=QObject::connect(&client,&ILLMClient::contextPrepared,[&](const QString&,const QString&,const QJsonArray&){client.generatePrepared();});
            auto received=QObject::connect(&client,&ILLMClient::answerReady,[&](const QString& value){result=value;done=true;loop.quit();});
            auto failed=QObject::connect(&client,&ILLMClient::errorOccurred,[&](const QString& value){error=value;done=true;loop.quit();});
            request.question=question;client.prepare(request);QTimer::singleShot(120000,&loop,&QEventLoop::quit);if(!done)loop.exec();
            QObject::disconnect(ready);QObject::disconnect(received);QObject::disconnect(failed);
            check(done && error.isEmpty(),qPrintable(error));qInfo().noquote()<<question<<result;return result;
        };
        check(answer(QStringLiteral("Как меня зовут?")).contains(QStringLiteral("Алексей"),Qt::CaseInsensitive),"Old user fact forgotten");
        request.history={{QStringLiteral("Уточнение: меня зовут Лиза, прежнее имя было ошибкой."),QStringLiteral("Принято.")}};
        const auto corrected=answer(QStringLiteral("Как меня зовут?"));
        check(corrected.contains(QStringLiteral("Лиза"),Qt::CaseInsensitive) && !corrected.contains(QStringLiteral("Алексей"),Qt::CaseInsensitive),"Old memory overrode correction");
        request.history.clear();const auto unrelated=answer(QStringLiteral("Сколько будет два плюс два?"));
        check(!unrelated.contains(QStringLiteral("Алексей"),Qt::CaseInsensitive) && !unrelated.contains(QStringLiteral("интернет"),Qt::CaseInsensitive),"Unrelated memory leaked");
        request.memory={};const auto unknown=answer(QStringLiteral("Как меня зовут?"));
        check(!unknown.contains(QStringLiteral("Алексей"),Qt::CaseInsensitive) && !unknown.contains(QStringLiteral("Лиза"),Qt::CaseInsensitive),"Memory leaked to empty chat");
        qInfo()<<"Real-model memory, corrections, topic change and isolation passed";
    }catch(const std::exception& e){qCritical()<<e.what();return 1;}return 0;
}
