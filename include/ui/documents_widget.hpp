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
private:
    QPushButton* uploadButton_ = nullptr;
};
