#pragma once

#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QFrame;
class QHideEvent;

class ServerStatusWidget final : public QWidget {
    Q_OBJECT
public:
    explicit ServerStatusWidget(QWidget* parent = nullptr);
    void setChatStatus(const QString& text);
    void setEmbeddingStatus(const QString& text);
    void setStorageStatus(const QString& text);
    void appendLog(const QString& server, const QString& text);
    void setSystemUsage(double cpuPercent, double memoryPercent);

protected:
    void hideEvent(QHideEvent* event) override;

private:
    QLabel* chatLabel_ = nullptr;
    QLabel* embeddingLabel_ = nullptr;
    QLabel* storageLabel_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    QLabel* usageLabel_ = nullptr;
    QPushButton* summaryButton_ = nullptr;
    QFrame* details_ = nullptr;
    void updateSummary();
    void positionDetails();
    void setStatus(QLabel* label, const QString& name, const QString& text);
};
