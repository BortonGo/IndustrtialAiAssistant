#include "message_composer.hpp"

#include <QTextEdit>
#include <QPushButton>
#include <QHBoxLayout>

MessageComposer::MessageComposer(QWidget* parent) : QWidget(parent) {
    textQuestion_ = new QTextEdit;

    textQuestion_->setPlaceholderText("Ask your question");
    textQuestion_->setStyleSheet(
                "QTextEdit {"
                " background-color: #24262B;"
                " color: #E8E8ED;"
                " border: 1px solid #393C44;"
                " border-radius: 14px;"
                " padding: 12px;"
                " font-size: 18px;"
                "}"
                );
    textQuestion_->setMinimumHeight(80);
    textQuestion_->setMaximumHeight(110);

    QPushButton *btnAsk = new QPushButton("↑");
    btnAsk->setFixedSize(44,44);
    btnAsk->setToolTip("Send question");
    btnAsk->setStyleSheet(
                "QPushButton {"
                " background-color: #1E90FF;"
                " color: #E8E8ED;"
                " border: none;"
                " border-radius: 22px;"
                " padding: 0px;"
                " padding-bottom: 4px;"
                " font-size: 26px;"
                "}"
                "QPushButton:hover { background-color: #46A3FF; }"
                "QPushButton:disabled {"
                " background-color: #393C44;"
                " color: #777B85;"
                "}"
                );

    QPushButton *btnLoadDoc = new QPushButton("+");
    btnLoadDoc->setFixedSize(44,44);
    btnLoadDoc->setToolTip("Load document");
    btnLoadDoc->setStyleSheet(
                "QPushButton {"
                " background-color: #393C44;"
                " color: #E8E8ED;"
                " border: none;"
                " border-radius: 22px;"
                " padding: 0px;"
                " padding-bottom: 4px;"
                " font-size: 26px;"
                "}"
                "QPushButton:hover { background-color: #494D57; }"
                );

    auto *hl = new QHBoxLayout(this);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(16);

    hl->addWidget(btnLoadDoc);
    hl->addWidget(textQuestion_);
    hl->addWidget(btnAsk);

    connect(btnLoadDoc, &QPushButton::clicked,
            this, &MessageComposer::documentUploadRequested);

    connect(btnAsk, &QPushButton::clicked,
            this, [this]() {
        emit sendRequested(textQuestion_->toPlainText());
    });
}

void MessageComposer::clearInput() {
    textQuestion_->clear();
}
