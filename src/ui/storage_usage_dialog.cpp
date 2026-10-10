#include "ui/storage_usage_dialog.hpp"
#include "storage/storage_client.hpp"
#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QJsonArray>

namespace {
QString bytes(double value) {
    const QStringList units{QStringLiteral("Б"), QStringLiteral("КиБ"), QStringLiteral("МиБ"), QStringLiteral("ГиБ"), QStringLiteral("ТиБ")};
    int index = 0;
    while (value >= 1024 && index < units.size()-1) { value /= 1024; ++index; }
    return QString::number(value, 'f', index ? 1 : 0) + " " + units[index];
}
}

StorageUsageDialog::StorageUsageDialog(StorageClient* storage, QWidget* parent)
    : QDialog(parent), storage_(storage) {
    setObjectName("storageUsage"); setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("Хранилище")); resize(640, 600); setMinimumSize(550, 500);
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(24, 22, 24, 20); layout->setSpacing(16);
    auto* title = new QLabel(QStringLiteral("Хранилище приложения"));
    title->setStyleSheet("font-size: 20px; font-weight: 600;"); layout->addWidget(title);
    directory_ = new QLabel; directory_->setTextFormat(Qt::PlainText); directory_->setWordWrap(true);
    directory_->setTextInteractionFlags(Qt::TextSelectableByMouse); directory_->setStyleSheet("color: #A5A6AA;");
    layout->addWidget(directory_);
    auto* card = new QWidget; card->setStyleSheet("background: #242527; border-radius: 8px;");
    metrics_ = new QGridLayout(card); metrics_->setContentsMargins(16, 14, 16, 14); metrics_->setHorizontalSpacing(24); metrics_->setVerticalSpacing(10);
    const QStringList labels{QStringLiteral("Всего в папке хранения"), QStringLiteral("PostgreSQL · весь кластер"),
        QStringLiteral("База приложения в кластере"), QStringLiteral("Сохранённые изображения"),
        QStringLiteral("Журналы и прочие файлы"), QStringLiteral("Свободно на диске"), QStringLiteral("Можно очистить")};
    for(int i=0; i<labels.size(); ++i) {
        auto* label = new QLabel(labels[i]); label->setWordWrap(true); metrics_->addWidget(label,i,0);
        auto* value = new QLabel(QStringLiteral("—")); value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        metrics_->addWidget(value,i,1);
    }
    metrics_->setColumnStretch(0,1); layout->addWidget(card);
    counts_ = new QLabel; counts_->setObjectName("storageCounts"); counts_->setWordWrap(true); layout->addWidget(counts_);
    auto* note = new QLabel(QStringLiteral("Очистка удаляет только неиспользуемые копии изображений и остатки их сохранения. Чаты, библиотека, источники ответов, незавершённый импорт и исходные файлы сохраняются.\n\nPostgreSQL повторно использует место удалённых записей: размер базы на диске может не уменьшиться. Кэш распознавания и модели находятся отдельно и здесь не учитываются."));
    note->setWordWrap(true); note->setStyleSheet("color: #A5A6AA;"); layout->addWidget(note);
    status_ = new QLabel; status_->setObjectName("storageUsageStatus"); status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true); layout->addWidget(status_); layout->addStretch();
    auto* actions = new QHBoxLayout;
    refresh_ = new QPushButton(QStringLiteral("Обновить")); refresh_->setObjectName("refreshStorageUsage");
    cleanup_ = new QPushButton(QStringLiteral("Очистить…")); cleanup_->setObjectName("cleanupStorage");
    actions->addWidget(refresh_); actions->addStretch(); actions->addWidget(cleanup_); layout->addLayout(actions);
    connect(refresh_,&QPushButton::clicked,this,[this](){refresh();});
    connect(cleanup_,&QPushButton::clicked,this,[this](){confirmCleanup();});
    connect(storage,&StorageClient::unavailable,this,[this](const QString& error){status_->setText(error); updateActions();});
    connect(storage,&StorageClient::ready,this,[this](){refresh();});
    refresh();
}

void StorageUsageDialog::updateActions() {
    const bool ready = !busy_ && storage_ && storage_->isReady();
    refresh_->setEnabled(ready);
    cleanup_->setEnabled(ready && report_.value("reclaimableFiles").toInt()>0 && report_.value("blockedReason").toString().isEmpty());
}

void StorageUsageDialog::refresh() {
    if(busy_ || !storage_) return;
    busy_=true; report_={}; updateActions(); status_->setText(QStringLiteral("Считаю размер и проверяю ссылки на изображения…"));
    QPointer<StorageUsageDialog> guard(this);
    storage_->request("storage_usage",{},[guard](const QJsonObject& result,const QString& error){
        if(!guard) return;
        guard->busy_=false;
        if(!error.isEmpty()) { guard->status_->setText(error); guard->updateActions(); return; }
        guard->report_=result; guard->directory_->setText(result.value("directory").toString());
        const QStringList keys{"totalBytes","clusterBytes","databaseBytes","imageBytes","otherBytes","freeBytes","reclaimableBytes"};
        for(int i=0; i<keys.size(); ++i) {
            auto* label=qobject_cast<QLabel*>(guard->metrics_->itemAtPosition(i,1)->widget());
            label->setText(bytes(result.value(keys[i]).toDouble()));
        }
        const auto counts=result.value("counts").toObject();
        guard->counts_->setText(QStringLiteral("Чатов: %1 · Документов в чатах: %2 · В библиотеке: %3\nФрагментов индекса: %4 · Заданий импорта: %5\nФайлов изображений: %6 · Не используются: %7")
            .arg(counts.value("chats").toInt()).arg(counts.value("documents").toInt()).arg(counts.value("library").toInt())
            .arg(counts.value("chunks").toInt()).arg(counts.value("imports").toInt())
            .arg(result.value("imageFiles").toInt()).arg(result.value("reclaimableFiles").toInt()));
        QStringList messages;
        if(!guard->notice_.isEmpty()) messages.append(guard->notice_);
        guard->notice_.clear();
        const auto reason=result.value("blockedReason").toString();
        if(!reason.isEmpty()) messages.append(reason);
        else if(!result.value("reclaimableFiles").toInt()) messages.append(QStringLiteral("Неиспользуемых копий изображений нет."));
        if(result.value("approximate").toBool()) messages.append(QStringLiteral("Размер неполный: часть файлов недоступна или является ссылками."));
        QStringList warnings; for(const auto& value:result.value("warnings").toArray()) warnings.append(value.toString());
        guard->status_->setToolTip(warnings.join("\n")); guard->status_->setText(messages.join("\n")); guard->updateActions();
    });
}

void StorageUsageDialog::confirmCleanup() {
    if(!cleanup_->isEnabled()) return;
    const auto token=report_.value("cleanupToken").toString();
    auto* dialog=new QDialog(this); dialog->setObjectName("storageCleanupConfirmation"); dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Очистить неиспользуемые файлы?"));
    auto* layout=new QVBoxLayout(dialog); layout->setContentsMargins(22,20,22,20); layout->setSpacing(18);
    auto* message=new QLabel(QStringLiteral("Будет удалено до %1 файлов (%2). Перед удалением приложение ещё раз проверит, что они нигде не используются. Исходные документы и переписки останутся.")
        .arg(report_.value("reclaimableFiles").toInt()).arg(bytes(report_.value("reclaimableBytes").toDouble())));
    message->setWordWrap(true); message->setMinimumWidth(380); layout->addWidget(message);
    auto* actions=new QHBoxLayout;
    auto* cancel=new QPushButton(QStringLiteral("Отмена")); cancel->setDefault(true);
    auto* confirm=new QPushButton(QStringLiteral("Очистить")); confirm->setObjectName("confirmStorageCleanup");
    actions->addStretch(); actions->addWidget(cancel); actions->addWidget(confirm); layout->addLayout(actions);
    connect(cancel,&QPushButton::clicked,dialog,&QDialog::reject);
    connect(confirm,&QPushButton::clicked,dialog,&QDialog::accept);
    busy_=true; updateActions();
    connect(dialog,&QDialog::finished,this,[this,token](int choice){
        if(choice!=QDialog::Accepted || !storage_) {busy_=false; updateActions(); return;}
        status_->setText(QStringLiteral("Проверяю ссылки и очищаю…"));
        QPointer<StorageUsageDialog> guard(this);
        storage_->request("cleanup_storage",{{"token",token}},[guard](const QJsonObject& result,const QString& error){
            if(!guard) return;
            guard->busy_=false;
            guard->notice_=error.isEmpty() ? QStringLiteral("Удалено файлов: %1. Освобождено: %2.")
                .arg(result.value("deletedFiles").toInt()).arg(bytes(result.value("freedBytes").toDouble())) : error;
            if(!result.value("warnings").toArray().isEmpty()) guard->notice_+=QStringLiteral(" Часть файлов пропущена: ")+result.value("warnings").toArray().first().toString();
            guard->refresh();
        });
    });
    dialog->open();
}
