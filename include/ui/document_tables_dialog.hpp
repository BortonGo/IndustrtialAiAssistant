#pragma once
#include <QDialog>
#include <QPointer>
#include <QJsonObject>

class StorageClient;
class QLabel;
class QComboBox;
class QTableView;
class QTextEdit;
class QPushButton;
class StoredTableModel;

// Writes only a completed export; failures preserve an existing destination.
bool saveTableExport(const QString& path, const QJsonObject& result, QString* error);

class DocumentTablesDialog : public QDialog {
public:
    DocumentTablesDialog(StorageClient* storage, const QString& chatId,
                         const QString& documentId, QWidget* parent = nullptr);
private:
    QPointer<StorageClient> storage_;
    QString chatId_, documentId_;
    QJsonObject report_;
    QPointer<QDialog> preview_;
    QLabel* title_;
    QLabel* status_;
    QComboBox* tables_;
    QTableView* view_;
    StoredTableModel* model_;
    QTextEdit* warnings_;
    QPushButton* source_;
    QPushButton* csv_;
    QPushButton* xlsx_;
    QPushButton* refresh_;
    bool busy_ = false;
    void load();
    void selectTable();
    void updateActions();
    void exportSelected(const QString& format);
    QJsonObject selectedTable() const;
};
