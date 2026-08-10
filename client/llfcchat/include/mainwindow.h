#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include "global.h"

class ChatDialog;
class LoginDialog;
class QTimer;
class RegisterDialog;
class ResetDialog;
/******************************************************************************
 *
 * @file       mainwindow.h
 * @brief      主界面功能 Function
 *
 * @author     恋恋风辰
 * @date       2024/02/27
 * @history
 *****************************************************************************/
namespace Ui {
class MainWindow;
}

enum UIStatus{
    LOGIN_UI,
    REGISTER_UI,
    RESET_UI,
    CHAT_UI
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();
public slots:
    void SlotSwitchReg();
    void SlotSwitchLogin();
    void SlotSwitchReset();
    void SlotSwitchLogin2();
    void SlotSwitchChat();
    void SlotOffline();
    void SlotExcepConOffline();
    void SlotResServerConOffline();
    void SlotReconnectHttpFinish(ReqId id, QString res, ErrorCodes err);
    void SlotReconnectFinished(bool success);

private:
    void offlineLogin();
    void scheduleReconnectAttempt();
    void attemptReconnect();
    void failReconnectAttempt();
    void finishReconnectExhausted();
    void cancelReconnect();
    Ui::MainWindow *ui;
    LoginDialog* _login_dlg;
    RegisterDialog* _reg_dlg;
    ResetDialog* _reset_dlg;
    ChatDialog* _chat_dlg;
    UIStatus _ui_status;
    QTimer* _reconnect_timer;
    int _reconnect_attempt;
    bool _reconnect_in_progress;
    bool _reconnect_http_pending;
    bool _reconnect_tcp_pending;
};

#endif // MAINWINDOW_H
