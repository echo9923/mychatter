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

bool MysqlMgr::UpdateUploadStatus(int chat_messag_id)
{
	return _dao.UpdateUploadStatus(chat_messag_id);
}

std::shared_ptr<ChatMessage> MysqlMgr::GetChatMsgById(int message_id)
{
	return _dao.GetChatMsgById(message_id);
}
