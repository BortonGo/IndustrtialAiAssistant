#pragma once
#include "documents/document.hpp"
#include <QObject>
#include <QByteArray>

class QProcess;
class QTimer;

struct DocumentExtractionConfig {
    QString pythonPath;
    QString scriptPath;
    QString popplerDirectory;
    QString visionUrl;
    QString visionProfile;
    QString sofficePath;
    qint64 maxFileSize = 100 * 1024 * 1024;
    int inactivityTimeoutMs = 360000;
};

bool parseDocumentContent(const QByteArray& json, Document& document, QString& error);

class DocumentExtractor final : public QObject {
    Q_OBJECT
public:
    explicit DocumentExtractor(const DocumentExtractionConfig& config, QObject* parent = nullptr);
    ~DocumentExtractor() override;
    void extract(const QString& path);
    void cancel();
signals:
    void contentReady(const Document& document);
    void errorOccurred(const QString& message);
    void progressChanged(const QString& message);
private:
    DocumentExtractionConfig config_;
    QProcess* process_ = nullptr;
    QTimer* timer_ = nullptr;
    QByteArray output_, stderrBuffer_, errorTail_;
    QString failure_;
    bool active_ = false;
    void* job_ = nullptr;
    void readOutput();
    void readErrors();
    void killWorker();
    void closeJob();
};
