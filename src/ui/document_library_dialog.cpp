#include "document_library_dialog.hpp"
#include "application/assistant_service.hpp"
#include "storage/storage_client.hpp"
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTimer>
#include <QFileInfo>
#include <QDateTime>
#include <QJsonArray>

DocumentLibraryDialog::DocumentLibraryDialog(AssistantService* service, const QString& chatId,
                                           const QString& publishDocumentId, QWidget* parent)
    : QDialog(parent), service_(service), chatId_(chatId) {
    setAttribute(Qt::WA_DeleteOnClose); setObjectName("documentLibrary");
    setWindowTitle(QStringLiteral("Библиотека документов")); resize(820, 580); setMinimumSize(640, 430);
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(24, 22, 24, 20); layout->setSpacing(14);
    auto* title = new QLabel(QStringLiteral("Библиотека документов")); title->setStyleSheet("font-size: 20px; font-weight: 600;"); layout->addWidget(title);
    auto* note = new QLabel(QStringLiteral("Сохранённые версии для повторного использования. Добавьте нужные документы в этот чат — без повторного распознавания. Содержимое других чатов не участвует в ответах автоматически."));
    note->setWordWrap(true); note->setStyleSheet("color: #A5A6AA;"); layout->addWidget(note);
    search_ = new QLineEdit; search_->setObjectName("librarySearch"); search_->setPlaceholderText(QStringLiteral("Найти по имени или пути файла…")); search_->setMaxLength(300);
    search_->setStyleSheet("QLineEdit { background: #242527; border: 1px solid #363739; border-radius: 8px; padding: 10px; }"); layout->addWidget(search_);
    list_ = new QListWidget; list_->setObjectName("libraryDocuments"); list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list_->setTextElideMode(Qt::ElideMiddle); list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setStyleSheet("QListWidget { background: #191A1B; border: 1px solid #363739; border-radius: 8px; } QListWidget::item { padding: 12px; border-bottom: 1px solid #292B2D; } QListWidget::item:selected { background: #323437; } QListWidget::item:disabled { color: #A5A6AA; }"); layout->addWidget(list_,1);
    status_ = new QLabel; status_->setObjectName("libraryStatus"); status_->setTextFormat(Qt::PlainText); status_->setWordWrap(true); status_->setMinimumHeight(38); layout->addWidget(status_);
    auto* actions = new QHBoxLayout;
    remove_ = new QPushButton(QStringLiteral("Удалить из библиотеки")); remove_->setObjectName("deleteLibraryDocument");
    refresh_ = new QPushButton(QStringLiteral("Обновить"));
    add_ = new QPushButton(QStringLiteral("Добавить в чат")); add_->setObjectName("attachLibraryDocuments");
    add_->setStyleSheet("QPushButton { background: #343638; border: 1px solid #515357; padding: 9px 14px; border-radius: 8px; } QPushButton:disabled { color: #77797D; }");
    actions->addWidget(remove_); actions->addWidget(refresh_); actions->addStretch(); actions->addWidget(add_); layout->addLayout(actions);
    searchTimer_ = new QTimer(this); searchTimer_->setSingleShot(true); searchTimer_->setInterval(250);
    connect(search_, &QLineEdit::textChanged, this, [this]() { searchTimer_->start(); });
    connect(searchTimer_, &QTimer::timeout, this, [this]() { load(); });
    connect(refresh_, &QPushButton::clicked, this, [this]() { load(); });
    connect(list_, &QListWidget::itemSelectionChanged, this, [this]() { updateActions(); });
    connect(add_, &QPushButton::clicked, this, [this]() { attach(); });
    connect(remove_, &QPushButton::clicked, this, [this]() { remove(); });
    connect(service, &AssistantService::busyChanged, this, [this]() { updateActions(); });
    connect(service->storage(), &StorageClient::unavailable, this, [this](const QString&) { updateActions(); });
    if (publishDocumentId.isEmpty()) load();
    else {
        busy_=true; status_->setText(QStringLiteral("Сохраняю версию в библиотеку…")); updateActions();
        QPointer<DocumentLibraryDialog> guard(this);
        service->storage()->request("publish_library_document", {{"chatId",chatId_},{"documentId",publishDocumentId}},
            [guard](const QJsonObject& result,const QString& error) {
            if (!guard) return;
            guard->busy_=false;
            guard->notice_=error.isEmpty() ? (result.value("existing").toBool() ? QStringLiteral("Эта версия уже есть в библиотеке.") : QStringLiteral("Версия сохранена. Её можно использовать в других чатах.")) : error;
            guard->load();
        });
    }
}

QStringList DocumentLibraryDialog::selectedIds() const {
    QStringList ids; for (auto* item : list_->selectedItems()) ids.append(item->data(Qt::UserRole).toString()); return ids;
}

void DocumentLibraryDialog::updateActions() {
    const auto items=list_->selectedItems();
    const bool ready=!busy_ && service_ && service_->storage()->isReady();
    bool compatible=true; for(auto* item:items) compatible=compatible && item->data(Qt::UserRole+1).toBool();
    add_->setEnabled(ready && !items.isEmpty() && items.size()<=20 && compatible && !service_->isBusy() && service_->isChatReady(chatId_));
    remove_->setEnabled(ready && items.size()==1); refresh_->setEnabled(ready); search_->setEnabled(!busy_);
    list_->setEnabled(!busy_);
}

void DocumentLibraryDialog::load() {
    if (busy_ || !service_) return;
    searchTimer_->stop(); busy_=true; updateActions();
    const auto version=++requestVersion_; const auto selected=selectedIds();
    QPointer<DocumentLibraryDialog> guard(this);
    service_->storage()->request("list_library", {{"search",search_->text()}}, [guard,version,selected](const QJsonObject& result,const QString& error) {
        if(!guard || guard->requestVersion_!=version) return;
        guard->busy_=false; guard->list_->clear();
        if(!error.isEmpty()) { guard->status_->setText(error); guard->updateActions(); return; }
        for(const auto& value:result.value("items").toArray()) {
            const auto item=value.toObject();
            const auto date=QDateTime::fromString(item.value("createdAt").toString(),Qt::ISODate).toLocalTime().toString("dd.MM.yyyy HH:mm");
            const auto info=QStringLiteral("%1 символов · %2 · версия %3").arg(item.value("characters").toInt()).arg(date,item.value("sourceHash").toString().left(8));
            auto* row=new QListWidgetItem(QFileInfo(item.value("sourcePath").toString()).fileName()+"\n"+info+
                (item.value("compatible").toBool() ? QString() : QStringLiteral("\nНужна переиндексация с текущей моделью")),guard->list_);
            row->setData(Qt::UserRole,item.value("id").toString()); row->setData(Qt::UserRole+1,item.value("compatible").toBool());
            row->setToolTip(item.value("sourcePath").toString()+"\n"+info);
            row->setSelected(selected.contains(item.value("id").toString()));
        }
        const auto hint=result.value("hasMore").toBool() ? QStringLiteral("Показаны первые 200 документов. Уточните поиск.") :
            (guard->list_->count() ? QStringLiteral("Выберите документы с Ctrl. Удаление из библиотеки сохраняет копии в чатах.") : QStringLiteral("Документов нет. Выберите готовый файл в «Документах чата» и нажмите «В библиотеку»."));
        guard->status_->setText(guard->notice_.isEmpty() ? hint : guard->notice_+"\n"+hint); guard->notice_.clear(); guard->updateActions();
    });
}

void DocumentLibraryDialog::attach() {
    if(!add_->isEnabled() || !service_) return;
    const auto count=selectedIds().size(); busy_=true; updateActions(); status_->setText(QStringLiteral("Добавляю сохранённые документы…"));
    QPointer<DocumentLibraryDialog> guard(this);
    service_->attachLibraryDocuments(chatId_,selectedIds(),[guard,count](const QString& error) {
        if(!guard) return;
        guard->busy_=false; guard->notice_=error.isEmpty() ? QStringLiteral("Документы добавлены в чат: %1. Можно задавать вопросы.").arg(count) : error;
        guard->load();
    });
}

void DocumentLibraryDialog::remove() {
    if(!remove_->isEnabled() || !service_) return;
    const auto ids=selectedIds();
    auto* confirmation=new QDialog(this);
    confirmation->setObjectName("removeLibraryConfirmation"); confirmation->setAttribute(Qt::WA_DeleteOnClose);
    confirmation->setWindowTitle(QStringLiteral("Удалить из библиотеки?")); confirmation->setMinimumWidth(420);
    auto* layout=new QVBoxLayout(confirmation); layout->setContentsMargins(22,20,22,20); layout->setSpacing(18);
    auto* message=new QLabel(QStringLiteral("Сохранённая версия будет удалена из библиотеки. Копии в чатах и исходный файл останутся."));
    message->setWordWrap(true); message->setMinimumWidth(360); layout->addWidget(message);
    auto* buttons=new QHBoxLayout;
    auto* cancel=new QPushButton(QStringLiteral("Отмена")); cancel->setDefault(true);
    auto* confirm=new QPushButton(QStringLiteral("Удалить")); confirm->setObjectName("confirmLibraryDeletion");
    buttons->addStretch(); buttons->addWidget(cancel); buttons->addWidget(confirm); layout->addLayout(buttons);
    connect(cancel,&QPushButton::clicked,confirmation,&QDialog::reject);
    connect(confirm,&QPushButton::clicked,confirmation,&QDialog::accept);
    busy_=true; updateActions();
    connect(confirmation,&QDialog::finished,this,[this,ids](int choice) {
        if(choice!=QDialog::Accepted || !service_) { busy_=false; updateActions(); return; }
        QPointer<DocumentLibraryDialog> guard(this);
        service_->storage()->request("delete_library_document",{{"id",ids.first()}},[guard](const QJsonObject& result,const QString& error) {
            if(!guard) return;
            guard->busy_=false; guard->notice_=error.isEmpty() ? QStringLiteral("Документ удалён из библиотеки.") : error;
            for(const auto& warning:result.value("warnings").toArray()) guard->notice_+="\n"+warning.toString();
            guard->load();
        });
    });
    confirmation->open();
}
