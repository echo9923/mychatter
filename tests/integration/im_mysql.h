// MySQL verification helpers for the current chat/event schema.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <jdbc/cppconn/connection.h>
#include <jdbc/mysql_connection.h>

namespace imt {

struct ChatMessageRow {
	std::int64_t message_id = 0;
	std::int64_t thread_id = 0;
	int sender_user_id = 0;
	std::string client_message_id;
	int message_type = 0;
	std::string text_content;
	int status = 0;
	std::string created_at;
	std::string original_file_name;
	std::uint64_t file_size_bytes = 0;
	std::string sha256;
	std::string mime_type;
};

struct UserEventRow {
	std::uint64_t event_seq = 0;
	int event_type = 0;
	std::uint64_t message_id = 0;
	std::uint64_t friend_request_id = 0;
};

struct FriendshipState {
	bool exists = false;
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

	std::vector<ChatMessageRow> QueryByClientMessageId(
		int senderUserId, const std::string& clientMessageId);
	std::vector<ChatMessageRow> QueryMessageById(std::int64_t messageId);
	long long CountByClientMessageId(const std::string& clientMessageId);
	long long CountByClientMessageIdLike(const std::string& pattern);
	long long CountPublishedByClientMessageIdLike(const std::string& pattern);
	long long DeleteTestDataByClientIdLike(const std::string& pattern);

	bool SnapshotFriendship(int firstUserId, int secondUserId,
		FriendshipState& state);
	bool RemoveFriendship(int firstUserId, int secondUserId);
	bool RestoreFriendship(int firstUserId, int secondUserId,
		const FriendshipState& state);
	long long CountFriendship(int firstUserId, int secondUserId);

	std::vector<UserEventRow> QueryUserEvents(std::int64_t userId,
		std::uint64_t afterEventSeq, int limit);
	std::uint64_t LastEventSeq(std::int64_t userId);

	long long GetChatMessagesAutoIncrement();
	bool SetChatMessagesAutoIncrement(std::int64_t value);
	bool BackdateMessageCreatedAt(std::int64_t messageId, int daysAgo);

private:
	sql::Connection* con_ = nullptr;
};

} // namespace imt
