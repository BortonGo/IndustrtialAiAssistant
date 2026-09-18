#pragma once

#include <QObject>
#include <QAbstractItemModel>
#include <QWidget>
#include <QTextEdit>
#include <QString>

class MessageComposer : public QWidget {
    Q_OBJECT

    QTextEdit* textQuestion_;
public:
    explicit MessageComposer(QWidget* parent = nullptr);
    void clearInput();

signals:
    void sendRequested(const QString& text);
    void documentUploadRequested();
};

