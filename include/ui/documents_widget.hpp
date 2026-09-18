#pragma once

#include <QObject>
#include <QAbstractItemModel>
#include <QWidget>

class DocumentsWidget : public QWidget {
    Q_OBJECT
public:
    explicit DocumentsWidget(QAbstractItemModel* model, QWidget* parent = nullptr);

signals:
    void documentUploadRequested();
};
