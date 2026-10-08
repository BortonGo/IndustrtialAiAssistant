#pragma once

#include <QObject>
#include <QAbstractItemModel>
#include <QWidget>
#include <QTextEdit>
#include <QString>

class QPushButton;
class QScrollArea;
class QHBoxLayout;

class MessageComposer : public QWidget {
    Q_OBJECT

    QTextEdit* textQuestion_;
    QPushButton* sendButton_ = nullptr;
    QPushButton* uploadButton_ = nullptr;
    QScrollArea* attachmentsArea_ = nullptr;
    QHBoxLayout* attachmentsLayout_ = nullptr;
    QStringList attachmentPaths_;
    quint64 attachmentRevision_ = 0;
public:
    explicit MessageComposer(QWidget* parent = nullptr);
    void clearInput();
    QString inputText() const;
    void setInputText(const QString& text);
    void setSendEnabled(bool enabled);
    void setUploadEnabled(bool enabled);
    QStringList attachmentPaths() const { return attachmentPaths_; }
    void setAttachmentPaths(const QStringList& paths);

signals:
    void sendRequested(const QString& text);
    void documentUploadRequested();
};

