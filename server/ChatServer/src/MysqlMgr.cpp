#include "MysqlMgr.h"


MysqlMgr::~MysqlMgr() {

}

int MysqlMgr::RegUser(const std::string& name, const std::string& email, const std::string& pwd)
{
	return _dao.RegUser(name, email, pwd);
}

bool MysqlMgr::CheckEmail(const std::string& name, const std::string& email) {
	return _dao.CheckEmail(name, email);
}

bool MysqlMgr::UpdatePwd(const std::string& name, const std::string& pwd) {
	return _dao.UpdatePwd(name, pwd);
}

MysqlMgr::MysqlMgr() {
}

bool MysqlMgr::CheckPwd(const std::string& name, const std::string& pwd, UserInfo& userInfo) {
	return _dao.CheckPwd(name, pwd, userInfo);
}

FriendOperationResult MysqlMgr::AddFriendApply(int requester_user_id, int target_user_id,
	const std::string& request_message, const std::string& client_request_id,
	std::shared_ptr<FriendRequest>& request) {
	return _dao.AddFriendApply(requester_user_id, target_user_id, request_message,
		client_request_id, request);
}

FriendOperationResult MysqlMgr::HandleFriendApply(int handler_user_id,
	std::int64_t friend_request_id, bool accept, FriendHandleOutput& output) {
	return _dao.HandleFriendApply(handler_user_id, friend_request_id, accept, output);
}

std::shared_ptr<UserInfo> MysqlMgr::GetUser(int uid)
{
	return _dao.GetUser(uid);
}

std::shared_ptr<UserInfo> MysqlMgr::GetUser(std::string name)
{
	return _dao.GetUser(name);
}

bool MysqlMgr::GetApplyList(int target_user_id,
	std::vector<std::shared_ptr<ApplyInfo>>& apply_list,
	std::int64_t after_friend_request_id, int limit) {

	return _dao.GetApplyList(target_user_id, apply_list, after_friend_request_id, limit);
}

bool MysqlMgr::GetFriendList(int user_id, std::vector<ContactInfo>& contacts) {
	return _dao.GetFriendList(user_id, contacts);
}

bool MysqlMgr::GetUserThreads(int64_t userId,
	int64_t lastId,
	int      pageSize,
	std::vector<std::shared_ptr<ChatThreadInfo>>& threads,
	bool& loadMore,
	int64_t& nextLastId)
{
	return _dao.GetUserThreads(userId, lastId, pageSize, threads, loadMore, nextLastId);
}

bool MysqlMgr::CreatePrivateChat(int requester_user_id, int target_user_id,
	std::int64_t& thread_id)
{
	return _dao.CreatePrivateChat(requester_user_id, target_user_id, thread_id);
}

bool MysqlMgr::GetPrivateChatMembers(std::int64_t thread_id,
	int& lower_user_id, int& higher_user_id)
{
	return _dao.GetPrivateChatMembers(thread_id, lower_user_id, higher_user_id);
}

std::shared_ptr<ChatMessage> MysqlMgr::GetChatMsgById(std::int64_t message_id)
{
	return _dao.GetChatMsgById(message_id);
}

std::shared_ptr<PageResult> MysqlMgr::LoadChatMsg(int requester_user_id,
	std::int64_t thread_id, std::int64_t before_message_id, int page_size)
{
	return _dao.LoadChatMsg(requester_user_id, thread_id, before_message_id, page_size);
}

SaveMessageResult MysqlMgr::AddChatMsg(std::shared_ptr<ChatMessage> chat_data) {
	return _dao.AddChatMsg(chat_data);
}

bool MysqlMgr::GetEventsAfterSeq(int user_id, std::uint64_t after_event_seq, int limit,
	std::vector<UserEvent>& events) {
	return _dao.GetEventsAfterSeq(user_id, after_event_seq, limit, events);
}

bool MysqlMgr::GetUserEvent(int recipient_user_id, int event_type,
	std::int64_t message_id, std::int64_t friend_request_id, UserEvent& event) {
	return _dao.GetUserEvent(recipient_user_id, event_type, message_id,
		friend_request_id, event);
}

bool MysqlMgr::GetLastEventSeq(int user_id, std::uint64_t& last_event_seq) {
	return _dao.GetLastEventSeq(user_id, last_event_seq);
}

std::shared_ptr<FriendRequest> MysqlMgr::GetFriendRequestById(
	std::int64_t friend_request_id) {
	return _dao.GetFriendRequestById(friend_request_id);
}

