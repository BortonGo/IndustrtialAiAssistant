#pragma once

#include <QObject>
#include <QAbstractItemModel>
#include <QWidget>
#include <QJsonArray>

class QPushButton;
class QListWidget;
class QLabel;
class QListView;

class DocumentsWidget : public QWidget {
    Q_OBJECT
public:
    explicit DocumentsWidget(QAbstractItemModel* model, QWidget* parent = nullptr);
    void setUploadEnabled(bool enabled);
    void setComparisonEnabled(bool enabled);
    void setImportJobs(const QJsonArray& jobs, const QString& chatId);

signals:
    void documentUploadRequested();
    void sidebarToggleRequested();
    void chatPageRequested();
    void importActionRequested(const QString& id, const QString& action);
    void compareRequested(const QStringList& documentIds);
    void tablesRequested(const QString& documentId);
private:
    QListView* documents_ = nullptr;
    QPushButton* compare_ = nullptr;
    QPushButton* tables_ = nullptr;
    bool comparisonEnabled_ = false;
    void updateComparisonAction();
    QPushButton* uploadButton_ = nullptr;
    QListWidget* importsList_ = nullptr;
    QLabel* importsLabel_ = nullptr;
    QPushButton* pauseImport_ = nullptr;
    QPushButton* resumeImport_ = nullptr;
    QPushButton* cancelImport_ = nullptr;
    void updateImportActions();
};
