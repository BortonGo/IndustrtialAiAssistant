#pragma once

#include <QWidget>

class QLabel;
class QPlainTextEdit;

class ServerStatusWidget final : public QWidget {
    Q_OBJECT
public:
    explicit ServerStatusWidget(QWidget* parent = nullptr);
    void setChatStatus(const QString& text);
    void setEmbeddingStatus(const QString& text);
    void appendLog(const QString& server, const QString& text);

private:
    QLabel* chatLabel_ = nullptr;
    QLabel* embeddingLabel_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    void setStatus(QLabel* label, const QString& name, const QString& text);
};
