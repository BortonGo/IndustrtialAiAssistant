#pragma once

#include <QObject>
#include <QAbstractItemModel>
#include <QWidget>

class QPushButton;

class DocumentsWidget : public QWidget {
    Q_OBJECT
public:
    explicit DocumentsWidget(QAbstractItemModel* model, QWidget* parent = nullptr);
    void setUploadEnabled(bool enabled);

signals:
    void documentUploadRequested();
    void sidebarToggleRequested();
    void chatPageRequested();
private:
    QPushButton* uploadButton_ = nullptr;
};
