#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct UserInfo {
	int user_id = 0;
	std::string username;
	std::string email;
	std::string nickname;
	std::string profile_bio;
	int gender = 0;
	std::string avatar_key;
};

enum class MessageStatus {
	Pending = 0,
	Published = 1,
	Failed = 2
};

struct MessageResource {
	std::int64_t message_id = 0;
	std::string original_file_name;
	std::uint64_t file_size_bytes = 0;
	std::string sha256;
	std::string mime_type;
};

struct ChatMessage {
	std::int64_t message_id = 0;
	std::int64_t thread_id = 0;
	int sender_user_id = 0;
	int recipient_user_id = 0; // Derived from private_chats; not persisted in chat_messages.
	std::string client_message_id;
	int message_type = 0;
	MessageStatus status = MessageStatus::Pending;
	std::string created_at;
	std::shared_ptr<MessageResource> resource;
	std::uint64_t event_seq = 0; // user_events cursor, populated for published messages.
};

struct ExpiredResource {
	std::int64_t message_id = 0;
	int sender_user_id = 0;
};

struct PageResult {
	std::vector<ChatMessage> messages;
	bool load_more = false;
	std::int64_t next_cursor = 0;
};
