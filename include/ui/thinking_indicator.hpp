#pragma once

#include <QWidget>
#include <QElapsedTimer>

class QTimer;

// A small, self-contained activity bubble. Hidden chats consume no animation ticks.
class ThinkingIndicator final : public QWidget {
    QTimer* timer_;
    QElapsedTimer elapsed_;
    bool writing_ = false;
protected:
    void paintEvent(QPaintEvent*) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
public:
    explicit ThinkingIndicator(QWidget* parent = nullptr);
    void setWriting(bool writing) {
        writing_ = writing;
        setAccessibleName(writing ? QStringLiteral("Пишу ответ") : QStringLiteral("Готовлю ответ"));
        update();
    }
};
