#pragma once
#include <QDialog>
#include <QPointer>
#include <QStringList>

class AssistantService;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTimer;

class DocumentLibraryDialog : public QDialog {
public:
    DocumentLibraryDialog(AssistantService* service, const QString& chatId,
                          const QString& publishDocumentId = {}, QWidget* parent = nullptr);
private:
    QPointer<AssistantService> service_;
    QString chatId_;
    QLabel* status_;
    QLineEdit* search_;
    QListWidget* list_;
    QPushButton* add_;
    QPushButton* remove_;
    QPushButton* refresh_;
    QTimer* searchTimer_;
    bool busy_ = false;
    quint64 requestVersion_ = 0;
    QString notice_;
    void load();
    void updateActions();
    void attach();
    void remove();
    QStringList selectedIds() const;
};
