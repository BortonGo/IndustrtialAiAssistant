#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class ILLMClient : public QObject {
    Q_OBJECT
public:
    explicit ILLMClient(QObject* parent = nullptr);
    virtual void generate(const QString& context,
                          const QString& question, const QStringList& images = {}) = 0;
    virtual void cancelRequests() = 0;
signals:
    void answerReady(const QString &answer);
    void errorOccurred(const QString &message);
};
