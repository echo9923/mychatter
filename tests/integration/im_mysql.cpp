// MySQL verification helpers for the current chat/event schema.
#include "im_mysql.h"

#include <algorithm>
#include <cstdio>
#include <memory>

#include <jdbc/cppconn/exception.h>
#include <jdbc/cppconn/prepared_statement.h>
#include <jdbc/cppconn/resultset.h>
#include <jdbc/cppconn/statement.h>
#include <jdbc/mysql_driver.h>

namespace imt {
namespace {

const char* kMessageColumns =
	"m.message_id,m.thread_id,m.sender_user_id,m.client_message_id,"
	"m.message_type,m.text_content,m.status,m.created_at,"
	"r.original_file_name,r.file_size_bytes,r.sha256,r.mime_type ";

ChatMessageRow ReadMessageRow(sql::ResultSet& result) {
	ChatMessageRow row;
	row.message_id = result.getInt64("message_id");
	row.thread_id = result.getInt64("thread_id");
	row.sender_user_id = result.getInt("sender_user_id");
	row.client_message_id = result.getString("client_message_id");
	row.message_type = result.getInt("message_type");
	row.text_content = result.isNull("text_content")
		? std::string() : result.getString("text_content");
	row.status = result.getInt("status");
	row.created_at = result.getString("created_at");
	row.original_file_name = result.isNull("original_file_name")
		? std::string() : result.getString("original_file_name");
	row.file_size_bytes = result.isNull("file_size_bytes")
		? 0 : static_cast<std::uint64_t>(result.getInt64("file_size_bytes"));
	row.sha256 = result.isNull("sha256")
		? std::string() : result.getString("sha256");
	row.mime_type = result.isNull("mime_type")
		? std::string() : result.getString("mime_type");
	return row;
}

void OrderedPair(int first, int second, int& lower, int& higher) {
	lower = std::min(first, second);
	higher = std::max(first, second);
}

} // namespace

Mysql::~Mysql() { Close(); }

bool Mysql::Connect(const std::string& host, int port, const std::string& user,
	const std::string& password, const std::string& schema) {
	Close();
	try {
		auto* driver = sql::mysql::get_mysql_driver_instance();
		con_ = driver->connect(
			"tcp://" + host + ":" + std::to_string(port), user, password);
		if (!con_) return false;
		con_->setSchema(schema);
		return true;
	} catch (const sql::SQLException& error) {
		std::printf("[mysql] connect failed: %s (code=%d)\n",
			error.what(), error.getErrorCode());
		return false;
	}
}

void Mysql::Close() {
	delete con_;
	con_ = nullptr;
}

std::vector<ChatMessageRow> Mysql::QueryByClientMessageId(
	int senderUserId, const std::string& clientMessageId) {
	std::vector<ChatMessageRow> output;
	if (!con_) return output;
	try {
		auto statement = std::unique_ptr<sql::PreparedStatement>(
			con_->prepareStatement(std::string("SELECT ") + kMessageColumns +
				"FROM chat_messages m LEFT JOIN message_resources r "
				"ON r.message_id=m.message_id "
				"WHERE m.sender_user_id=? AND m.client_message_id=?"));
		statement->setInt(1, senderUserId);
		statement->setString(2, clientMessageId);
		auto rows = std::unique_ptr<sql::ResultSet>(statement->executeQuery());
		while (rows->next()) output.push_back(ReadMessageRow(*rows));
	} catch (const sql::SQLException& error) {
		std::printf("[mysql] QueryByClientMessageId failed: %s (code=%d)\n",
			error.what(), error.getErrorCode());
	}
	return output;
}

std::vector<ChatMessageRow> Mysql::QueryMessageById(std::int64_t messageId) {
	std::vector<ChatMessageRow> output;
	if (!con_) return output;
	try {
		auto statement = std::unique_ptr<sql::PreparedStatement>(
			con_->prepareStatement(std::string("SELECT ") + kMessageColumns +
				"FROM chat_messages m LEFT JOIN message_resources r "
				"ON r.message_id=m.message_id WHERE m.message_id=?"));
		statement->setInt64(1, messageId);
		auto rows = std::unique_ptr<sql::ResultSet>(statement->executeQuery());
		while (rows->next()) output.push_back(ReadMessageRow(*rows));
	} catch (const sql::SQLException& error) {
		std::printf("[mysql] QueryMessageById failed: %s (code=%d)\n",
			error.what(), error.getErrorCode());
	}
	return output;
}

long long Mysql::CountByClientMessageId(const std::string& clientMessageId) {
	if (!con_) return -1;
	try {
		auto statement = std::unique_ptr<sql::PreparedStatement>(con_->prepareStatement(
			"SELECT COUNT(*) AS c FROM chat_messages WHERE client_message_id=?"));
		statement->setString(1, clientMessageId);
		auto rows = std::unique_ptr<sql::ResultSet>(statement->executeQuery());
		return rows->next() ? rows->getInt64("c") : 0;
	} catch (const sql::SQLException& error) {
		std::printf("[mysql] CountByClientMessageId failed: %s (code=%d)\n",
			error.what(), error.getErrorCode());
		return -1;
	}
}

long long Mysql::CountByClientMessageIdLike(const std::string& pattern) {
	if (!con_) return -1;
	try {
		auto statement = std::unique_ptr<sql::PreparedStatement>(con_->prepareStatement(
			"SELECT COUNT(*) AS c FROM chat_messages WHERE client_message_id LIKE ?"));
		statement->setString(1, pattern);
		auto rows = std::unique_ptr<sql::ResultSet>(statement->executeQuery());
		return rows->next() ? rows->getInt64("c") : 0;
	} catch (const sql::SQLException& error) {
		std::printf("[mysql] CountByClientMessageIdLike failed: %s (code=%d)\n",
			error.what(), error.getErrorCode());
		return -1;
	}
}

long long Mysql::CountPublishedByClientMessageIdLike(const std::string& pattern) {
	if (!con_) return -1;
	try {
		auto statement = std::unique_ptr<sql::PreparedStatement>(con_->prepareStatement(
			"SELECT COUNT(*) AS c FROM chat_messages "
			"WHERE client_message_id LIKE ? AND status=1"));
		statement->setString(1, pattern);
		auto rows = std::unique_ptr<sql::ResultSet>(statement->executeQuery());
		return rows->next() ? rows->getInt64("c") : 0;
	} catch (const sql::SQLException& error) {
		std::printf("[mysql] CountPublishedByClientMessageIdLike failed: %s (code=%d)\n",
			error.what(), error.getErrorCode());
		return -1;
	}
}

long long Mysql::DeleteTestDataByClientIdLike(const std::string& pattern) {
	if (!con_) return -1;
	try {
		con_->setAutoCommit(false);
		long long affected = 0;
		auto deleteMessageEvents = std::unique_ptr<sql::PreparedStatement>(
			con_->prepareStatement(
				"DELETE e FROM user_events e INNER JOIN chat_messages m "
				"ON m.message_id=e.message_id WHERE m.client_message_id LIKE ?"));
		deleteMessageEvents->setString(1, pattern);
		affected += deleteMessageEvents->executeUpdate();
		auto deleteFriendEvents = std::unique_ptr<sql::PreparedStatement>(
			con_->prepareStatement(
				"DELETE e FROM user_events e INNER JOIN friend_requests f "
				"ON f.friend_request_id=e.friend_request_id "
				"WHERE f.client_request_id LIKE ?"));
		deleteFriendEvents->setString(1, pattern);
		affected += deleteFriendEvents->executeUpdate();
		auto deleteMessages = std::unique_ptr<sql::PreparedStatement>(
			con_->prepareStatement(
				"DELETE FROM chat_messages WHERE client_message_id LIKE ?"));
		deleteMessages->setString(1, pattern);
		affected += deleteMessages->executeUpdate();
		auto deleteRequests = std::unique_ptr<sql::PreparedStatement>(
			con_->prepareStatement(
				"DELETE FROM friend_requests WHERE client_request_id LIKE ?"));
		deleteRequests->setString(1, pattern);
		affected += deleteRequests->executeUpdate();
		con_->commit();
		con_->setAutoCommit(true);
		return affected;
	} catch (const sql::SQLException& error) {
		try { con_->rollback(); con_->setAutoCommit(true); } catch (...) {}
		std::printf("[mysql] DeleteTestDataByClientIdLike failed: %s (code=%d)\n",
			error.what(), error.getErrorCode());
		return -1;
	}
}

bool Mysql::SnapshotFriendship(int firstUserId, int secondUserId,
	FriendshipState& state) {
	state = FriendshipState{};
	if (!con_) return false;
	try {
		int lower = 0, higher = 0;
		OrderedPair(firstUserId, secondUserId, lower, higher);
		auto statement = std::unique_ptr<sql::PreparedStatement>(con_->prepareStatement(
			"SELECT 1 FROM friendships WHERE lower_user_id=? AND higher_user_id=?"));
		statement->setInt(1, lower);
		statement->setInt(2, higher);
		auto rows = std::unique_ptr<sql::ResultSet>(statement->executeQuery());
		state.exists = rows->next();
		return true;
	} catch (const sql::SQLException& error) {
		std::printf("[mysql] SnapshotFriendship failed: %s (code=%d)\n",
			error.what(), error.getErrorCode());
		return false;
	}
}

bool Mysql::RemoveFriendship(int firstUserId, int secondUserId) {
	if (!con_) return false;
	try {
		int lower = 0, higher = 0;
		OrderedPair(firstUserId, secondUserId, lower, higher);
		auto statement = std::unique_ptr<sql::PreparedStatement>(con_->prepareStatement(
			"DELETE FROM friendships WHERE lower_user_id=? AND higher_user_id=?"));
		statement->setInt(1, lower);
		statement->setInt(2, higher);
		statement->executeUpdate();
		return true;
	} catch (const sql::SQLException& error) {
		std::printf("[mysql] RemoveFriendship failed: %s (code=%d)\n",
			error.what(), error.getErrorCode());
		return false;
	}
}

bool Mysql::RestoreFriendship(int firstUserId, int secondUserId,
	const FriendshipState& state) {
	if (!RemoveFriendship(firstUserId, secondUserId)) return false;
	if (!state.exists) return true;
	try {
		int lower = 0, higher = 0;
		OrderedPair(firstUserId, secondUserId, lower, higher);
		auto statement = std::unique_ptr<sql::PreparedStatement>(con_->prepareStatement(
			"INSERT INTO friendships(lower_user_id,higher_user_id) VALUES(?,?)"));
		statement->setInt(1, lower);
		statement->setInt(2, higher);
		statement->executeUpdate();
		return true;
	} catch (const sql::SQLException& error) {
		std::printf("[mysql] RestoreFriendship failed: %s (code=%d)\n",
			error.what(), error.getErrorCode());
		return false;
	}
}

long long Mysql::CountFriendship(int firstUserId, int secondUserId) {
	FriendshipState state;
	return SnapshotFriendship(firstUserId, secondUserId, state)
		? (state.exists ? 1 : 0) : -1;
}

std::vector<UserEventRow> Mysql::QueryUserEvents(std::int64_t userId,
	std::uint64_t afterEventSeq, int limit) {
	std::vector<UserEventRow> output;
	if (!con_) return output;
	try {
		std::string query =
			"SELECT event_seq,event_type,message_id,friend_request_id "
			"FROM user_events WHERE recipient_user_id=? AND event_seq>? "
			"ORDER BY event_seq ASC";
		if (limit > 0) query += " LIMIT " + std::to_string(limit);
		auto statement = std::unique_ptr<sql::PreparedStatement>(
			con_->prepareStatement(query));
		statement->setInt64(1, userId);
		statement->setUInt64(2, afterEventSeq);
		auto rows = std::unique_ptr<sql::ResultSet>(statement->executeQuery());
		while (rows->next()) {
			UserEventRow row;
			row.event_seq = static_cast<std::uint64_t>(rows->getInt64("event_seq"));
			row.event_type = rows->getInt("event_type");
			row.message_id = rows->isNull("message_id")
				? 0 : static_cast<std::uint64_t>(rows->getInt64("message_id"));
			row.friend_request_id = rows->isNull("friend_request_id")
				? 0 : static_cast<std::uint64_t>(rows->getInt64("friend_request_id"));
			output.push_back(row);
		}
	} catch (const sql::SQLException& error) {
		std::printf("[mysql] QueryUserEvents failed: %s (code=%d)\n",
			error.what(), error.getErrorCode());
	}
	return output;
}

std::uint64_t Mysql::LastEventSeq(std::int64_t userId) {
	if (!con_) return 0;
	try {
		auto statement = std::unique_ptr<sql::PreparedStatement>(con_->prepareStatement(
			"SELECT last_event_seq FROM users WHERE user_id=?"));
		statement->setInt64(1, userId);
		auto rows = std::unique_ptr<sql::ResultSet>(statement->executeQuery());
		return rows->next()
			? static_cast<std::uint64_t>(rows->getInt64("last_event_seq")) : 0;
	} catch (const sql::SQLException& error) {
		std::printf("[mysql] LastEventSeq failed: %s (code=%d)\n",
			error.what(), error.getErrorCode());
		return 0;
	}
}

long long Mysql::GetChatMessagesAutoIncrement() {
	if (!con_) return -1;
	try {
		auto statement = std::unique_ptr<sql::PreparedStatement>(con_->prepareStatement(
			"SELECT AUTO_INCREMENT FROM information_schema.TABLES "
			"WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='chat_messages'"));
		auto rows = std::unique_ptr<sql::ResultSet>(statement->executeQuery());
		return rows->next() ? rows->getInt64("AUTO_INCREMENT") : -1;
	} catch (const sql::SQLException& error) {
		std::printf("[mysql] GetChatMessagesAutoIncrement failed: %s (code=%d)\n",
			error.what(), error.getErrorCode());
		return -1;
	}
}

bool Mysql::SetChatMessagesAutoIncrement(std::int64_t value) {
	if (!con_) return false;
	try {
		auto statement = std::unique_ptr<sql::Statement>(con_->createStatement());
		statement->execute(
			"ALTER TABLE chat_messages AUTO_INCREMENT=" + std::to_string(value));
		return true;
	} catch (const sql::SQLException& error) {
		std::printf("[mysql] SetChatMessagesAutoIncrement failed: %s (code=%d)\n",
			error.what(), error.getErrorCode());
		return false;
	}
}

bool Mysql::BackdateMessageCreatedAt(std::int64_t messageId, int daysAgo) {
	if (!con_) return false;
	try {
		auto statement = std::unique_ptr<sql::PreparedStatement>(con_->prepareStatement(
			"UPDATE chat_messages SET created_at=DATE_SUB(NOW(),INTERVAL ? DAY) "
			"WHERE message_id=?"));
		statement->setInt(1, daysAgo);
		statement->setInt64(2, messageId);
		statement->executeUpdate();
		return true;
	} catch (const sql::SQLException& error) {
		std::printf("[mysql] BackdateMessageCreatedAt failed: %s (code=%d)\n",
			error.what(), error.getErrorCode());
		return false;
	}
}

} // namespace imt
