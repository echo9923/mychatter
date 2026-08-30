#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct UserInfo {
	UserInfo() = default;
	int user_id{0};
	std::string username;
	std::string email;
	std::string nickname;
	std::string profile_bio;
	int gender{0};
	std::string avatar_key;
};

struct ContactInfo {
	std::shared_ptr<UserInfo> user;
	std::int64_t thread_id{0};
};

enum class ChatMsgType {
	TEXT = 0,
	PIC = 1,
	FILE = 3
};

enum class UserEventType {
	FRIEND_APPLY = 10,
	FRIEND_ACCEPT = 11,
	FRIEND_REJECT = 12
};

enum class MessageStatus {
	Pending = 0,
	Published = 1,
	Failed = 2
};

enum class FriendRequestStatus {
	Pending = 0,
	Accepted = 1,
	Rejected = 2
};

struct MessageResource {
	std::int64_t message_id{0};
	std::string original_file_name;
	std::uint64_t file_size_bytes{0};
	std::string sha256;
	std::string mime_type;
};

struct ChatMessage {
	std::int64_t message_id{0};
	std::int64_t thread_id{0};
	int sender_user_id{0};
	int recipient_user_id{0}; // Derived from private_chats; never persisted.
	std::string client_message_id;
	int message_type{static_cast<int>(ChatMsgType::TEXT)};
	std::string text_content;
	MessageStatus status{MessageStatus::Published};
	std::string created_at;
	std::shared_ptr<MessageResource> resource;
	std::uint64_t event_seq{0}; // user_events projection; never persisted.
};

struct FriendRequest {
	std::int64_t friend_request_id{0};
	int requester_user_id{0};
	int target_user_id{0};
	std::string client_request_id;
	std::string request_message;
	FriendRequestStatus status{FriendRequestStatus::Pending};
	std::int64_t thread_id{0};
	std::string created_at;
	std::uint64_t event_seq{0}; // user_events projection.
};

struct ApplyInfo {
	std::shared_ptr<FriendRequest> request;
	std::shared_ptr<UserInfo> peer;
};

struct ChatThreadInfo {
	std::int64_t _thread_id{0};
	int _lower_user_id{0};
	int _higher_user_id{0};
	std::int64_t _last_msg_id{0};
};

struct UserEvent {
	std::uint64_t event_seq{0};
	int event_type{0};
	std::shared_ptr<ChatMessage> message;
	std::shared_ptr<FriendRequest> friend_request;
};

struct PageResult {
	std::vector<ChatMessage> messages;
	bool load_more{false};
	std::int64_t next_cursor{0};
};
