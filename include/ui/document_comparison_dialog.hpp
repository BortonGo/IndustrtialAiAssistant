#pragma once
#include <QDialog>
#include <QJsonObject>
#include <QPointer>
#include <QStringList>

class StorageClient;
class QLabel;
class QTextEdit;
class QTableWidget;
class QPushButton;

// A read-only report of stored extraction snapshots; no model generation.
class DocumentComparisonDialog : public QDialog {
public:
    DocumentComparisonDialog(StorageClient* storage, const QString& chatId,
                             const QStringList& documentIds, QWidget* parent = nullptr);
private:
    QJsonObject report_;
    QLabel* summary_;
    QLabel* files_;
    QTextEdit* warnings_;
    QTableWidget* table_;
    QPushButton* swap_;
    QPushButton* before_;
    QPushButton* after_;
    bool reversed_ = false;
    QPointer<QDialog> preview_;
    void displayReport();
    void updateSelection();
    QJsonObject selectedSource(bool after) const;
    void openSource(bool after);
};
