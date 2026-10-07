#pragma once

#include <QObject>
#include <QAbstractItemModel>
#include <QWidget>
#include <QTextEdit>
#include <QString>

class QPushButton;

class MessageComposer : public QWidget {
    Q_OBJECT

    QTextEdit* textQuestion_;
    QPushButton* sendButton_ = nullptr;
    QPushButton* uploadButton_ = nullptr;
public:
    explicit MessageComposer(QWidget* parent = nullptr);
    void clearInput();
    void setSendEnabled(bool enabled);
    void setUploadEnabled(bool enabled);

signals:
    void sendRequested(const QString& text);
    void documentUploadRequested();
};

