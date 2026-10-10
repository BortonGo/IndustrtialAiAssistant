#pragma once
#include <QDialog>
#include <QJsonObject>
#include <QPointer>

class StorageClient;
class QLabel;
class QPushButton;
class QGridLayout;

class StorageUsageDialog : public QDialog {
public:
    explicit StorageUsageDialog(StorageClient* storage, QWidget* parent = nullptr);
private:
    QPointer<StorageClient> storage_;
    QLabel* directory_;
    QLabel* counts_;
    QLabel* status_;
    QGridLayout* metrics_;
    QPushButton* refresh_;
    QPushButton* cleanup_;
    QJsonObject report_;
    bool busy_ = false;
    QString notice_;
    void refresh();
    void confirmCleanup();
    void updateActions();
};
