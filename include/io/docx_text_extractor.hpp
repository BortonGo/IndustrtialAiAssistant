#pragma once

#include <QObject>
#include <QString>
#include <QByteArray>

class QProcess;
class QTimer;

class DocxTextExtractor final : public QObject {
    Q_OBJECT

    QString pythonPath_;
    QString scriptPath_;
    QProcess* process_ = nullptr;
    QByteArray outputBuffer_;
    QByteArray errorBuffer_;
    QTimer* timer_ = nullptr;
    bool timeOut_ = false;

public:
    explicit DocxTextExtractor(const QString& pythonPath,
                               const QString& scriptPath,
                               QObject* parent = nullptr);
    void extract(const QString& path);

signals:
    void textReady(const QString& text);
    void errorOccurred(const QString& message);
};
