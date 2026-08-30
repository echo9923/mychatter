#include "authenfriend.h"

#include <QJsonDocument>
#include <QJsonObject>

#include "tcpmgr.h"
#include "ui_authenfriend.h"

AuthenFriend::AuthenFriend(QWidget *parent)
    : QDialog(parent), ui(new Ui::AuthenFriend)
{
    ui->setupUi(this);
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
    setObjectName("AuthenFriend");
    setModal(true);

    ui->sure_btn->SetState("normal", "hover", "press");
    ui->reject_btn->SetState("normal", "hover", "press");
    ui->cancel_btn->SetState("normal", "hover", "press");

    connect(ui->sure_btn, &QPushButton::clicked,
            this, &AuthenFriend::SlotApplySure);
    connect(ui->reject_btn, &QPushButton::clicked,
            this, &AuthenFriend::SlotApplyReject);
    connect(ui->cancel_btn, &QPushButton::clicked,
            this, &AuthenFriend::SlotApplyCancel);
}

AuthenFriend::~AuthenFriend()
{
    delete ui;
}

void AuthenFriend::SetApplyInfo(std::shared_ptr<ApplyInfo> apply_info)
{
    _apply_info = apply_info;
    ui->applicant_name_lb->setText(apply_info ? apply_info->_name : QString());
    ui->request_message_lb->setText(apply_info ? apply_info->_desc : QString());
}

void AuthenFriend::SlotApplySure()
{
    if (!_apply_info) {
        return;
    }

    QJsonObject request;
    request["friend_request_id"] =
        QString::number(_apply_info->_friend_request_id);
    request["action"] = "accept";
    emit TcpMgr::GetInstance()->sig_send_data(
        ReqId::ID_HANDLE_FRIEND_REQ,
        QJsonDocument(request).toJson(QJsonDocument::Compact));
    hide();
    deleteLater();
}

void AuthenFriend::SlotApplyReject()
{
    if (!_apply_info) {
        return;
    }

    QJsonObject request;
    request["friend_request_id"] =
        QString::number(_apply_info->_friend_request_id);
    request["action"] = "reject";
    emit TcpMgr::GetInstance()->sig_send_data(
        ReqId::ID_HANDLE_FRIEND_REQ,
        QJsonDocument(request).toJson(QJsonDocument::Compact));
    hide();
    deleteLater();
}

void AuthenFriend::SlotApplyCancel()
{
    hide();
    deleteLater();
}
