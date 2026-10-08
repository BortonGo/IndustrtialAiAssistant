#pragma once

#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QPushButton;

class ServerStatusWidget final : public QWidget {
    Q_OBJECT
public:
    explicit ServerStatusWidget(QWidget* parent = nullptr);
    void setChatStatus(const QString& text);
    void setEmbeddingStatus(const QString& text);
    void setStorageStatus(const QString& text);
    void appendLog(const QString& server, const QString& text);
    void setDocumentProgress(const QString& text, bool canCancel);
signals:
    void cancelDocumentRequested();

private:
    QLabel* chatLabel_ = nullptr;
    QLabel* embeddingLabel_ = nullptr;
    QLabel* storageLabel_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    QLabel* documentLabel_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
    void setStatus(QLabel* label, const QString& name, const QString& text);
};
