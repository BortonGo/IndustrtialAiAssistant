#pragma once

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QTimer>

class QProcess;

class PdfTextExtractor : public QObject {
    Q_OBJECT

    QString executablePath_;
    QProcess* process_ = nullptr;
    QByteArray outputBuffer_;
    QTimer* timer_ = nullptr;
    bool timeOut_ = false;
public:
    explicit PdfTextExtractor(const QString& executablePath, QObject* parent = nullptr);
    void extract(const QString& pdfPath);

signals:
    void textReady(const QString& text);
    void errorOccurred(const QString& message);
};

