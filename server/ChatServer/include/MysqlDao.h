#pragma once

#include "MySqlPool.h"
#include "const.h"
#include "data.h"

#include <jdbc/cppconn/prepared_statement.h>
#include <jdbc/cppconn/resultset.h>
#include <jdbc/mysql_connection.h>

#include <memory>
#include <string>
#include <vector>

enum class SaveMessageResult {
	Stored,
	Duplicate,
	Conflict,
	Failed
};

enum class FriendOperationResult {
	Stored,
	Duplicate,
	Conflict,
	NotFound,
	AlreadyHandled,
	AlreadyFriends,
	Forbidden,
	Failed
};

struct FriendHandleOutput {
	std::shared_ptr<FriendRequest> request;
	std::int64_t thread_id{0};
	int peer_user_id{0};
};

class MysqlDao {
public:
	MysqlDao();
	~MysqlDao();

	int RegUser(const std::string& name, const std::string& email, const std::string& pwd);
	bool CheckEmail(const std::string& name, const std::string& email);
	bool UpdatePwd(const std::string& name, const std::string& newpwd);
	bool CheckPwd(const std::string& name, const std::string& pwd, UserInfo& user_info);

	FriendOperationResult AddFriendApply(int requester_user_id, int target_user_id,
		const std::string& request_message, const std::string& client_request_id,
		std::shared_ptr<FriendRequest>& request);
	FriendOperationResult HandleFriendApply(int handler_user_id,
		std::int64_t friend_request_id, bool accept, FriendHandleOutput& output);

	std::shared_ptr<UserInfo> GetUser(int user_id);
	std::shared_ptr<UserInfo> GetUser(const std::string& username);
	bool GetApplyList(int target_user_id,
		std::vector<std::shared_ptr<ApplyInfo>>& requests,
		std::int64_t after_friend_request_id, int limit);
	bool GetFriendList(int user_id, std::vector<ContactInfo>& contacts);

	bool GetUserThreads(std::int64_t user_id, std::int64_t after_thread_id, int page_size,
		std::vector<std::shared_ptr<ChatThreadInfo>>& threads,
		bool& load_more, std::int64_t& next_thread_id);
	bool CreatePrivateChat(int requester_user_id, int target_user_id,
		std::int64_t& thread_id);
	bool GetPrivateChatMembers(std::int64_t thread_id, int& lower_user_id,
		int& higher_user_id);

	std::shared_ptr<PageResult> LoadChatMsg(int requester_user_id,
		std::int64_t thread_id, std::int64_t before_message_id, int page_size);
	SaveMessageResult AddChatMsg(const std::shared_ptr<ChatMessage>& message);
	bool GetEventsAfterSeq(int recipient_user_id, std::uint64_t after_event_seq, int limit,
		std::vector<UserEvent>& events);
	bool GetUserEvent(int recipient_user_id, int event_type, std::int64_t message_id,
		std::int64_t friend_request_id, UserEvent& event);
	bool GetLastEventSeq(int user_id, std::uint64_t& last_event_seq);
	std::shared_ptr<ChatMessage> GetChatMsgById(std::int64_t message_id);
	std::shared_ptr<FriendRequest> GetFriendRequestById(std::int64_t friend_request_id);

private:
	SaveMessageResult AddChatMsgTransaction(sql::Connection* connection,
		const std::shared_ptr<ChatMessage>& message);
	bool AllocateEventSeq(sql::Connection* connection, int recipient_user_id,
		std::uint64_t& event_seq);
	bool InsertMessageEvent(sql::Connection* connection, int recipient_user_id,
		std::uint64_t event_seq, int event_type, std::int64_t message_id);
	bool InsertFriendEvent(sql::Connection* connection, int recipient_user_id,
		std::uint64_t event_seq, int event_type, std::int64_t friend_request_id);
	bool ReadPrivateChatMembers(sql::Connection* connection, std::int64_t thread_id,
		int& lower_user_id, int& higher_user_id, bool for_update = false);
	std::shared_ptr<ChatMessage> ReadMessage(sql::ResultSet* result);
	std::shared_ptr<FriendRequest> ReadFriendRequest(sql::ResultSet* result);
	std::shared_ptr<UserInfo> ReadUser(sql::ResultSet* result);

	std::unique_ptr<MySqlPool> pool_;
	int deadlock_retries_ = 2;
};
