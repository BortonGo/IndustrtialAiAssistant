#include "message_composer.hpp"

#include <QTextEdit>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>

MessageComposer::MessageComposer(QWidget* parent) : QWidget(parent) {
    setObjectName("messageComposer");
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(
        "QWidget#messageComposer {"
        " background-color: #24262B;"
        " border: 1px solid #393C44;"
        " border-radius: 18px;"
        "}"
    );

    textQuestion_ = new QTextEdit;

    textQuestion_->setPlaceholderText("Ask your question");
    textQuestion_->setStyleSheet(
        "QTextEdit {"
        " background-color: transparent;"
        " color: #E8E8ED;"
        " border: none;"
        " padding: 0px;"
        " font-size: 16px;"
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

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(8);

    auto* buttonsLayout = new QHBoxLayout;
    buttonsLayout->setContentsMargins(0, 0, 0, 0);
    buttonsLayout->setSpacing(8);

    buttonsLayout->addWidget(btnLoadDoc);
    buttonsLayout->addStretch();
    buttonsLayout->addWidget(btnAsk);

    layout->addWidget(textQuestion_);
    layout->addLayout(buttonsLayout);

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
