#include "applyfriendpage.h"
#include "ui_applyfriendpage.h"
#include <QPainter>
#include <QPaintEvent>
#include <QStyleOption>
#include "applyfrienditem.h"
#include "authenfriend.h"
#include "applyfriend.h"
#include "tcpmgr.h"
#include "usermgr.h"


ApplyFriendPage::ApplyFriendPage(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::ApplyFriendPage)
{
    ui->setupUi(this);
    connect(ui->apply_friend_list, &ApplyFriendList::sig_show_search, this, &ApplyFriendPage::sig_show_search);
    loadApplyList();
    //接受tcp传递的authrsp信号处理
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_auth_rsp, this, &ApplyFriendPage::slot_auth_rsp);
	connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_friend_request_handled,
		this, &ApplyFriendPage::slot_request_handled);
}

ApplyFriendPage::~ApplyFriendPage()
{
    delete ui;
}

void ApplyFriendPage::AddNewApply(std::shared_ptr<AddFriendApply> apply)
{
	//std::unordered_map::contains 是 C++20，客户端固定 C++11，用 count 判重
	if (!apply || apply->_friend_request_id <= 0
		|| _unauth_items.count(apply->_friend_request_id) > 0) {
		return;
	}
	auto* apply_item = new ApplyFriendItem();
    auto apply_info = std::make_shared<ApplyInfo>(apply->_from_uid,
		 apply->_name, apply->_desc, apply->_icon, apply->_nick, apply->_sex,
		 static_cast<int>(FriendRequestStatus::PENDING), apply->_friend_request_id);
    apply_item->SetInfo( apply_info);
	QListWidgetItem* item = new QListWidgetItem;
	//qDebug()<<"chat_user_wid sizeHint is " << chat_user_wid->sizeHint();
	item->setSizeHint(apply_item->sizeHint());
	item->setFlags(item->flags() & ~Qt::ItemIsEnabled & ~Qt::ItemIsSelectable);
	ui->apply_friend_list->insertItem(0,item);
	ui->apply_friend_list->setItemWidget(item, apply_item);
	apply_item->ShowStatus(FriendRequestStatus::PENDING);
	_unauth_items[apply->_friend_request_id] = apply_item;
	//收到审核好友信号
    connect(apply_item, &ApplyFriendItem::sig_auth_friend, [this](std::shared_ptr<ApplyInfo> apply_info) {
		auto* authFriend = new AuthenFriend(this);
		authFriend->setModal(true);
        authFriend->SetApplyInfo(apply_info);
		authFriend->show();
		});
}

void ApplyFriendPage::paintEvent(QPaintEvent *event)
{
    QStyleOption opt;
    opt.init(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}

void ApplyFriendPage::loadApplyList()
{
    //添加好友申请
    auto apply_list = UserMgr::GetInstance()->GetApplyList();
    for(auto &apply: apply_list){
        auto* apply_item = new ApplyFriendItem();
        apply_item->SetInfo(apply);
        QListWidgetItem* item = new QListWidgetItem;
        //qDebug()<<"chat_user_wid sizeHint is " << chat_user_wid->sizeHint();
        item->setSizeHint(apply_item->sizeHint());
        item->setFlags(item->flags() & ~Qt::ItemIsEnabled & ~Qt::ItemIsSelectable);
        ui->apply_friend_list->insertItem(0,item);
        ui->apply_friend_list->setItemWidget(item, apply_item);
		const auto status = static_cast<FriendRequestStatus>(apply->_status);
		apply_item->ShowStatus(status);
		if (status == FriendRequestStatus::PENDING) {
			_unauth_items[apply_item->GetFriendRequestId()] = apply_item;
		}

        //收到审核好友信号
        connect(apply_item, &ApplyFriendItem::sig_auth_friend, [this](std::shared_ptr<ApplyInfo> apply_info) {
            auto* authFriend = new AuthenFriend(this);
            authFriend->setModal(true);
            authFriend->SetApplyInfo(apply_info);
            authFriend->show();
            });
    }
}

void ApplyFriendPage::slot_auth_rsp(std::shared_ptr<AuthRsp> auth_rsp) {
	Q_UNUSED(auth_rsp);
}

void ApplyFriendPage::slot_request_handled(qint64 friendRequestId, int status)
{
	auto found = _unauth_items.find(friendRequestId);
	if (found == _unauth_items.end()) return;
	found->second->ShowStatus(static_cast<FriendRequestStatus>(status));
	_unauth_items.erase(found);
}



