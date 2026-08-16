#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "chatdialog.h"
#include "logindialog.h"
#include "registerdialog.h"
#include "resetdialog.h"
#include "tcpmgr.h"
#include <QMessageBox>
#include "filetcpmgr.h"
#include "usermgr.h"
#include "httpmgr.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QUrl>

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow),
    _login_dlg(nullptr),
    _reg_dlg(nullptr),
    _reset_dlg(nullptr),
    _chat_dlg(nullptr),
    _ui_status(LOGIN_UI),
    _reconnect_timer(new QTimer(this)),
    _reconnect_attempt(0),
    _reconnect_in_progress(false),
    _reconnect_http_pending(false),
    _reconnect_tcp_pending(false)
{
    ui->setupUi(this);
    _reconnect_timer->setSingleShot(true);
    connect(_reconnect_timer, &QTimer::timeout, this, &MainWindow::attemptReconnect);
    //创建一个CentralWidget, 并将其设置为MainWindow的中心部件
    _login_dlg = new LoginDialog(this);
    _login_dlg->setWindowFlags(Qt::CustomizeWindowHint|Qt::FramelessWindowHint);
    _login_dlg->show();
    setCentralWidget(_login_dlg);

    //连接登录界面注册信号
    connect(_login_dlg, &LoginDialog::switchRegister, this, &MainWindow::SlotSwitchReg);
    //连接登录界面忘记密码信号
    connect(_login_dlg, &LoginDialog::switchReset, this, &MainWindow::SlotSwitchReset);
    //连接创建聊天界面信号
    connect(TcpMgr::GetInstance().get(),&TcpMgr::sig_swich_chatdlg, this, &MainWindow::SlotSwitchChat);
    //链接服务器踢人消息
    connect(TcpMgr::GetInstance().get(),&TcpMgr::sig_notify_offline, this, &MainWindow::SlotOffline);
    //连接服务器断开心跳超时或异常连接信息
    connect(TcpMgr::GetInstance().get(),&TcpMgr::sig_connection_closed, this, &MainWindow::SlotExcepConOffline);
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_reconnect_finished,
            this, &MainWindow::SlotReconnectFinished);
    connect(HttpMgr::GetInstance().get(), &HttpMgr::sig_reconnect_mod_finish,
            this, &MainWindow::SlotReconnectHttpFinish);
    //连接资源服务器断开
    connect(FileTcpMgr::GetInstance().get(), &FileTcpMgr::sig_connection_closed,
            this, &MainWindow::SlotResServerConOffline);
}

MainWindow::~MainWindow()
{
    cancelReconnect();
    delete ui;
}

void MainWindow::SlotSwitchReg()
{
    cancelReconnect();
    _reg_dlg = new RegisterDialog(this);
    _reg_dlg->hide();

    _reg_dlg->setWindowFlags(Qt::CustomizeWindowHint|Qt::FramelessWindowHint);

     //连接注册界面返回登录信号
    connect(_reg_dlg, &RegisterDialog::sigSwitchLogin, this, &MainWindow::SlotSwitchLogin);
    setCentralWidget(_reg_dlg);
    _login_dlg->hide();
    _reg_dlg->show();
    _ui_status = REGISTER_UI;
}

//从注册界面返回登录界面
void MainWindow::SlotSwitchLogin()
{
    cancelReconnect();
    //创建一个CentralWidget, 并将其设置为MainWindow的中心部件
    _login_dlg = new LoginDialog(this);
    _login_dlg->setWindowFlags(Qt::CustomizeWindowHint|Qt::FramelessWindowHint);
    setCentralWidget(_login_dlg);

   _reg_dlg->hide();
    _login_dlg->show();
    //连接登录界面注册信号
    connect(_login_dlg, &LoginDialog::switchRegister, this, &MainWindow::SlotSwitchReg);
    //连接登录界面忘记密码信号
    connect(_login_dlg, &LoginDialog::switchReset, this, &MainWindow::SlotSwitchReset);
    _ui_status = LOGIN_UI;
}

void MainWindow::SlotSwitchReset()
{
    cancelReconnect();
    _ui_status = RESET_UI;
    //创建一个CentralWidget, 并将其设置为MainWindow的中心部件
    _reset_dlg = new ResetDialog(this);
    _reset_dlg->setWindowFlags(Qt::CustomizeWindowHint|Qt::FramelessWindowHint);
    setCentralWidget(_reset_dlg);

   _login_dlg->hide();
    _reset_dlg->show();
    //注册返回登录信号和槽函数
    connect(_reset_dlg, &ResetDialog::switchLogin, this, &MainWindow::SlotSwitchLogin2);
}

//从重置界面返回登录界面
void MainWindow::SlotSwitchLogin2()
{
    cancelReconnect();
    //创建一个CentralWidget, 并将其设置为MainWindow的中心部件
    _login_dlg = new LoginDialog(this);
    _login_dlg->setWindowFlags(Qt::CustomizeWindowHint|Qt::FramelessWindowHint);
    setCentralWidget(_login_dlg);

   _reset_dlg->hide();
    _login_dlg->show();
    //连接登录界面忘记密码信号
    connect(_login_dlg, &LoginDialog::switchReset, this, &MainWindow::SlotSwitchReset);
    //连接登录界面注册信号
    connect(_login_dlg, &LoginDialog::switchRegister, this, &MainWindow::SlotSwitchReg);
    _ui_status = LOGIN_UI;
}

void MainWindow::SlotSwitchChat()
{
    cancelReconnect();
    _chat_dlg = new ChatDialog();
    _chat_dlg->setWindowFlags(Qt::CustomizeWindowHint|Qt::FramelessWindowHint);
    setCentralWidget(_chat_dlg);
    _chat_dlg->show();
    _login_dlg->hide();
    this->setMinimumSize(QSize(1050,900));
    this->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    _ui_status = CHAT_UI;
    _chat_dlg->loadChatList();
}

void MainWindow::SlotOffline(){
    cancelReconnect();
    // 使用静态方法直接弹出一个信息框
        QMessageBox::information(this, "下线提示", "同账号异地登录，该终端下线！");
        TcpMgr::GetInstance()->CloseConnection();
        offlineLogin();
}

void MainWindow::SlotExcepConOffline()
{
    if (_ui_status == CHAT_UI) {
        if (_reconnect_in_progress) {
            return;
        }

        _reconnect_in_progress = true;
        _reconnect_attempt = 0;
        _reconnect_http_pending = false;
        _reconnect_tcp_pending = false;
        if (UserMgr::GetInstance()->GetUid() <= 0 ||
            UserMgr::GetInstance()->GetToken().isEmpty()) {
            finishReconnectExhausted();
            return;
        }
        scheduleReconnectAttempt();
        return;
    }

    cancelReconnect();
    QMessageBox::information(this, tr("Connection"),
                             tr("The chat connection was closed."));
    TcpMgr::GetInstance()->CloseConnection();
    FileTcpMgr::GetInstance()->CloseConnection();
    offlineLogin();
}


void MainWindow::SlotResServerConOffline(){
    cancelReconnect();
    if (_ui_status != CHAT_UI) {
        return;
    }
    // 使用静态方法直接弹出一个信息框
    QMessageBox::information(this, "下线提示", "与资源服务器断开连接！");
    TcpMgr::GetInstance()->CloseConnection();
    FileTcpMgr::GetInstance()->CloseConnection();
    offlineLogin();
}

void MainWindow::scheduleReconnectAttempt()
{
    static const int delays[] = {2000, 4000, 8000};
    if (!_reconnect_in_progress || _reconnect_http_pending ||
        _reconnect_tcp_pending) {
        return;
    }
    if (_reconnect_attempt >= 3) {
        finishReconnectExhausted();
        return;
    }

    _reconnect_timer->start(delays[_reconnect_attempt]);
}

void MainWindow::attemptReconnect()
{
    if (!_reconnect_in_progress || _reconnect_http_pending ||
        _reconnect_tcp_pending || _ui_status != CHAT_UI) {
        return;
    }

    ++_reconnect_attempt;
    const int uid = UserMgr::GetInstance()->GetUid();
    const QString token = UserMgr::GetInstance()->GetToken();
    if (uid <= 0 || token.isEmpty()) {
        failReconnectAttempt();
        return;
    }

    QJsonObject request;
    request["uid"] = uid;
    request["token"] = token;
    _reconnect_http_pending = true;
    HttpMgr::GetInstance()->PostHttpReq(
        QUrl(gate_url_prefix + "/reassign_chat"), request,
        ReqId::ID_REASSIGN_CHAT, Modules::RECONNECTMOD);
}

void MainWindow::SlotReconnectHttpFinish(ReqId id, QString res, ErrorCodes err)
{
    if (id != ReqId::ID_REASSIGN_CHAT || !_reconnect_in_progress ||
        !_reconnect_http_pending || _ui_status != CHAT_UI) {
        return;
    }

    _reconnect_http_pending = false;
    if (err != ErrorCodes::SUCCESS) {
        failReconnectAttempt();
        return;
    }

    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(res.toUtf8(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        failReconnectAttempt();
        return;
    }

    const QJsonObject response = document.object();
    if (!response.value("error").isDouble() ||
        response.value("error").toInt() != ErrorCodes::SUCCESS ||
        !response.value("chathost").isString() ||
        !response.value("chatport").isString()) {
        failReconnectAttempt();
        return;
    }

    const QString host = response.value("chathost").toString();
    bool port_ok = false;
    const uint port = response.value("chatport").toString().toUInt(&port_ok);
    if (host.isEmpty() || !port_ok ||
        port == 0 || port > 65535) {
        failReconnectAttempt();
        return;
    }

    _reconnect_tcp_pending = true;
    TcpMgr::GetInstance()->ReconnectChat(host, static_cast<quint16>(port));
}

void MainWindow::SlotReconnectFinished(bool success)
{
    if (!_reconnect_in_progress || !_reconnect_tcp_pending ||
        _ui_status != CHAT_UI) {
        return;
    }

    _reconnect_tcp_pending = false;
    if (success) {
        cancelReconnect();
        return;
    }
    failReconnectAttempt();
}

void MainWindow::failReconnectAttempt()
{
    _reconnect_http_pending = false;
    _reconnect_tcp_pending = false;
    if (!_reconnect_in_progress) {
        return;
    }
    if (_reconnect_attempt >= 3) {
        finishReconnectExhausted();
        return;
    }
    scheduleReconnectAttempt();
}

void MainWindow::finishReconnectExhausted()
{
    cancelReconnect();
    QMessageBox::information(
        this, tr("Connection"),
        tr("Chat server connection failed. Please log in again."));
    offlineLogin();
    TcpMgr::GetInstance()->CloseConnection();
    FileTcpMgr::GetInstance()->CloseConnection();
}

void MainWindow::cancelReconnect()
{
    _reconnect_timer->stop();
    _reconnect_attempt = 0;
    _reconnect_in_progress = false;
    _reconnect_http_pending = false;
    _reconnect_tcp_pending = false;
}

void MainWindow::offlineLogin(){
    cancelReconnect();
    if(_ui_status == LOGIN_UI){
        return;
    }
    //返回登录页清空内存中的登录 token，避免残留凭据
    UserMgr::GetInstance()->SetToken("");
    //创建一个CentralWidget, 并将其设置为MainWindow的中心部件
    _login_dlg = new LoginDialog(this);
    _login_dlg->setWindowFlags(Qt::CustomizeWindowHint|Qt::FramelessWindowHint);
    setCentralWidget(_login_dlg);

   if (_chat_dlg) {
       _chat_dlg->hide();
   }
   this->setMaximumSize(300,500);
   this->setMinimumSize(300,500);
   this->resize(300,500);
    _login_dlg->show();
    //连接登录界面注册信号
    connect(_login_dlg, &LoginDialog::switchRegister, this, &MainWindow::SlotSwitchReg);
    //连接登录界面忘记密码信号
    connect(_login_dlg, &LoginDialog::switchReset, this, &MainWindow::SlotSwitchReset);
    _ui_status = LOGIN_UI;
}
