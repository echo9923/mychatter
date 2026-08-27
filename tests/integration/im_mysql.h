// im_mysql.h - MySQL verification helpers for the unified user-message stream.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <jdbc/mysql_connection.h>
#include <jdbc/cppconn/connection.h>

namespace imt {

struct ChatMessageRow {
	std::int64_t message_id = 0;
	std::int64_t thread_id = 0;
	int sender_id = 0;
	int recv_id = 0;
	std::uint64_t recv_seq = 0; // 0 means SQL NULL (resource not published yet)
	std::string unique_id;
	std::string content;
	std::string chat_time;
	int status = 0;
	int msg_type = 0;
	int resource_status = 0;
	int business_status = 0;
	std::int64_t related_message_id = 0;
	std::string content_hash;
	std::uint64_t content_size = 0;
};

struct RecvRow {
	std::uint64_t recv_seq = 0;
	std::uint64_t message_id = 0;
};

struct FriendPairState {
	bool first_to_second = false;
	bool second_to_first = false;
	std::string first_remark;
	std::string second_remark;
};

class Mysql {
public:
	Mysql() = default;
	~Mysql();
	Mysql(const Mysql&) = delete;
	Mysql& operator=(const Mysql&) = delete;

	bool Connect(const std::string& host, int port, const std::string& user,
	             const std::string& pwd, const std::string& schema);
	void Close();
	bool connected() const { return con_ != nullptr; }

	std::vector<ChatMessageRow> QueryByUniqueId(int sender_id,
	                                            const std::string& unique_id);
	std::vector<ChatMessageRow> QueryByMessageId(std::int64_t message_id);
	long long CountByUniqueId(const std::string& unique_id);
	long long CountByUniqueIdLike(const std::string& pattern);
	long long CountVisibleByUniqueIdLike(const std::string& pattern);
	long long DeleteByUniqueIdLike(const std::string& pattern);
	bool SnapshotFriendPair(int first_uid, int second_uid, FriendPairState& state);
	bool RemoveFriendPair(int first_uid, int second_uid);
	bool RestoreFriendPair(int first_uid, int second_uid, const FriendPairState& state);
	long long CountFriendDirections(int first_uid, int second_uid);

	std::vector<RecvRow> QueryRecvRows(std::int64_t uid,
	                                   std::uint64_t after_recv_seq,
	                                   int limit);
	std::uint64_t LastRecvSeq(std::int64_t uid);

	long long GetChatMessageAutoIncrement();
	bool SetChatMessageAutoIncrement(std::int64_t value);
	bool BackdateMessageUpdatedAt(std::int64_t message_id, int days_ago);

private:
	sql::Connection* con_ = nullptr;
};

} // namespace imt
