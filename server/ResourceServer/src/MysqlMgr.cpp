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

bool MysqlMgr::CompleteResourceUpload(long long message_id, int sender_user_id,
	unsigned long long& event_seq)
{
	return _dao.CompleteResourceUpload(message_id, sender_user_id, event_seq);
}

std::shared_ptr<ChatMessage> MysqlMgr::GetChatMsgById(long long message_id)
{
	return _dao.GetChatMsgById(message_id);
}

bool MysqlMgr::GetExpiredResourceIds(const std::string& before_time, int limit,
	std::vector<ExpiredResource>& out)
{
	return _dao.GetExpiredResourceIds(before_time, limit, out);
}

bool MysqlMgr::MarkResourceExpired(const std::vector<ExpiredResource>& items)
{
	return _dao.MarkResourceExpired(items);
}
