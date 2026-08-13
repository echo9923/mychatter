#include "MysqlMgr.h"


MysqlMgr::~MysqlMgr() {

}

MysqlMgr::MysqlMgr() {
}

std::shared_ptr<UserInfo> MysqlMgr::GetUser(int uid)
{
	return _dao.GetUser(uid);
}

bool MysqlMgr::UpdateUserIcon(int uid, const std::string& icon) {
	return _dao.UpdateHeadInfo(uid, icon);
}

bool MysqlMgr::UpdateUploadStatusWithSync(long long chat_messag_id, int sender_id, int recv_id)
{
	return _dao.UpdateUploadStatusWithSync(chat_messag_id, sender_id, recv_id);
}

std::shared_ptr<ChatMessage> MysqlMgr::GetChatMsgById(long long message_id)
{
	return _dao.GetChatMsgById(message_id);
}
