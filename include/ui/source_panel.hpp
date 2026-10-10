#pragma once
#include <QWidget>
#include <QJsonArray>
#include <QPointer>

class QLabel;
class QListWidget;
class QTextEdit;
class QPushButton;
class QDialog;
class QJsonObject;

QDialog* showSourcePreview(const QJsonObject& source, QWidget* parent);

// Displays the evidence saved with a question, independent of the current index.
class SourcePanel : public QWidget {
    QLabel* question_;
    QListWidget* list_;
    QTextEdit* legacy_;
    QPushButton* open_;
    QJsonArray references_;
    QPointer<QDialog> preview_;
    void openSource(int index);
public:
    explicit SourcePanel(QWidget* parent = nullptr);
    void setSources(const QString& question, const QString& snapshot, const QJsonArray& references);
};
