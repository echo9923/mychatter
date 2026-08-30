#include "applyfriend.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

#include "tcpmgr.h"
#include "ui_applyfriend.h"
#include "usermgr.h"

ApplyFriend::ApplyFriend(QWidget *parent)
    : QDialog(parent), ui(new Ui::ApplyFriend)
{
    ui->setupUi(this);
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
    setObjectName("ApplyFriend");
    setModal(true);

    ui->name_ed->SetMaxLength(255);
    ui->sure_btn->SetState("normal", "hover", "press");
    ui->cancel_btn->SetState("normal", "hover", "press");

    connect(ui->sure_btn, &QPushButton::clicked,
            this, &ApplyFriend::SlotApplySure);
    connect(ui->cancel_btn, &QPushButton::clicked,
            this, &ApplyFriend::SlotApplyCancel);
}

ApplyFriend::~ApplyFriend()
{
    delete ui;
}

void ApplyFriend::SetSearchInfo(std::shared_ptr<SearchInfo> search_info)
{
    _search_info = search_info;
    ui->target_name_lb->setText(search_info ? search_info->_name : QString());
    ui->name_ed->setText(tr("你好，我是%1").arg(UserMgr::GetInstance()->GetName()));
}

void ApplyFriend::SlotApplySure()
{
    if (!_search_info) {
        return;
    }

    QJsonObject request;
    request["target_user_id"] = _search_info->_uid;
    request["client_request_id"] =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    request["request_message"] = ui->name_ed->text().trimmed();

    emit TcpMgr::GetInstance()->sig_send_data(
        ReqId::ID_ADD_FRIEND_REQ,
        QJsonDocument(request).toJson(QJsonDocument::Compact));
    hide();
    deleteLater();
}

void ApplyFriend::SlotApplyCancel()
{
    hide();
    deleteLater();
}
