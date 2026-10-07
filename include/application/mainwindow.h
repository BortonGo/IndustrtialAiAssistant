#pragma once
#include <QMainWindow>

class AssistantService;
class QCloseEvent;

namespace Ui {
class MainWindow;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = 0);
    ~MainWindow();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    Ui::MainWindow *ui;
    AssistantService* service_ = nullptr;
    bool shutdownRequested_ = false;
    bool shutdownComplete_ = false;

};
