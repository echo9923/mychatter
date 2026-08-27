// im_mysql.cpp - MySQL verification helpers for the unified user-message stream.
#include "im_mysql.h"

#include <cstdio>
#include <memory>

#include <jdbc/mysql_driver.h>
#include <jdbc/mysql_connection.h>
#include <jdbc/cppconn/exception.h>
#include <jdbc/cppconn/prepared_statement.h>
#include <jdbc/cppconn/resultset.h>
#include <jdbc/cppconn/statement.h>

namespace imt {
namespace {

const char* kMessageColumns =
	"message_id, thread_id, sender_id, recv_id, recv_seq, unique_id, content, "
	"created_at AS chat_time, status, msg_type, resource_status, business_status, "
	"related_message_id, content_hash, content_size ";

ChatMessageRow ReadMessageRow(sql::ResultSet& res) {
	ChatMessageRow row;
	row.message_id = res.getInt64("message_id");
	row.thread_id = res.isNull("thread_id") ? 0 : res.getInt64("thread_id");
	row.sender_id = res.getInt("sender_id");
	row.recv_id = res.getInt("recv_id");
	row.recv_seq = res.isNull("recv_seq")
		? 0 : static_cast<std::uint64_t>(res.getInt64("recv_seq"));
	row.unique_id = res.getString("unique_id");
	row.content = res.getString("content");
	row.chat_time = res.getString("chat_time");
	row.status = res.getInt("status");
	row.msg_type = res.getInt("msg_type");
	row.resource_status = res.getInt("resource_status");
	row.business_status = res.getInt("business_status");
	row.related_message_id = res.isNull("related_message_id")
		? 0 : res.getInt64("related_message_id");
	row.content_hash = res.isNull("content_hash") ? "" : res.getString("content_hash");
	row.content_size = static_cast<std::uint64_t>(res.getInt64("content_size"));
	return row;
}

} // namespace

Mysql::~Mysql() { Close(); }

bool Mysql::Connect(const std::string& host, int port, const std::string& user,
	                const std::string& pwd, const std::string& schema) {
	Close();
	try {
		auto* driver = sql::mysql::get_mysql_driver_instance();
		con_ = driver->connect("tcp://" + host + ":" + std::to_string(port), user, pwd);
		if (!con_) return false;
		con_->setSchema(schema);
		return true;
	} catch (const sql::SQLException& e) {
		std::printf("[mysql] connect failed: %s (code=%d)\n", e.what(), e.getErrorCode());
		return false;
	}
}

void Mysql::Close() {
	if (con_) {
		delete con_;
		con_ = nullptr;
	}
}

std::vector<ChatMessageRow> Mysql::QueryByUniqueId(
	int sender_id, const std::string& unique_id) {
	std::vector<ChatMessageRow> out;
	if (!con_) return out;
	try {
		auto stmt = std::unique_ptr<sql::PreparedStatement>(con_->prepareStatement(
			std::string("SELECT ") + kMessageColumns +
			"FROM chat_message WHERE sender_id = ? AND unique_id = ?"));
		stmt->setInt(1, sender_id);
		stmt->setString(2, unique_id);
		auto rows = std::unique_ptr<sql::ResultSet>(stmt->executeQuery());
		while (rows->next()) out.push_back(ReadMessageRow(*rows));
	} catch (const sql::SQLException& e) {
		std::printf("[mysql] QueryByUniqueId failed: %s (code=%d)\n", e.what(), e.getErrorCode());
	}
	return out;
}

std::vector<ChatMessageRow> Mysql::QueryByMessageId(std::int64_t message_id) {
	std::vector<ChatMessageRow> out;
	if (!con_) return out;
	try {
		auto stmt = std::unique_ptr<sql::PreparedStatement>(con_->prepareStatement(
			std::string("SELECT ") + kMessageColumns +
			"FROM chat_message WHERE message_id = ?"));
		stmt->setInt64(1, message_id);
		auto rows = std::unique_ptr<sql::ResultSet>(stmt->executeQuery());
		while (rows->next()) out.push_back(ReadMessageRow(*rows));
	} catch (const sql::SQLException& e) {
		std::printf("[mysql] QueryByMessageId failed: %s (code=%d)\n", e.what(), e.getErrorCode());
	}
	return out;
}

long long Mysql::CountByUniqueId(const std::string& unique_id) {
	if (!con_) return -1;
	try {
		auto stmt = std::unique_ptr<sql::PreparedStatement>(con_->prepareStatement(
			"SELECT COUNT(*) AS c FROM chat_message WHERE unique_id = ?"));
		stmt->setString(1, unique_id);
		auto rows = std::unique_ptr<sql::ResultSet>(stmt->executeQuery());
		return rows->next() ? rows->getInt64("c") : 0;
	} catch (const sql::SQLException& e) {
		std::printf("[mysql] CountByUniqueId failed: %s (code=%d)\n", e.what(), e.getErrorCode());
		return -1;
	}
}

long long Mysql::CountByUniqueIdLike(const std::string& pattern) {
	if (!con_) return -1;
	try {
		auto stmt = std::unique_ptr<sql::PreparedStatement>(con_->prepareStatement(
			"SELECT COUNT(*) AS c FROM chat_message WHERE unique_id LIKE ?"));
		stmt->setString(1, pattern);
		auto rows = std::unique_ptr<sql::ResultSet>(stmt->executeQuery());
		return rows->next() ? rows->getInt64("c") : 0;
	} catch (const sql::SQLException& e) {
		std::printf("[mysql] CountByUniqueIdLike failed: %s (code=%d)\n", e.what(), e.getErrorCode());
		return -1;
	}
}

long long Mysql::CountVisibleByUniqueIdLike(const std::string& pattern) {
	if (!con_) return -1;
	try {
		auto stmt = std::unique_ptr<sql::PreparedStatement>(con_->prepareStatement(
			"SELECT COUNT(*) AS c FROM chat_message "
			"WHERE unique_id LIKE ? AND recv_seq IS NOT NULL"));
		stmt->setString(1, pattern);
		auto rows = std::unique_ptr<sql::ResultSet>(stmt->executeQuery());
		return rows->next() ? rows->getInt64("c") : 0;
	} catch (const sql::SQLException& e) {
		std::printf("[mysql] CountVisibleByUniqueIdLike failed: %s (code=%d)\n",
			e.what(), e.getErrorCode());
		return -1;
	}
}

long long Mysql::DeleteByUniqueIdLike(const std::string& pattern) {
	if (!con_) return -1;
	try {
		auto results = std::unique_ptr<sql::PreparedStatement>(con_->prepareStatement(
			"DELETE r FROM chat_message r "
			"INNER JOIN chat_message a ON r.related_message_id = a.message_id "
			"WHERE a.unique_id LIKE ?"));
		results->setString(1, pattern);
		const long long result_rows = static_cast<long long>(results->executeUpdate());
		auto stmt = std::unique_ptr<sql::PreparedStatement>(con_->prepareStatement(
			"DELETE FROM chat_message WHERE unique_id LIKE ?"));
		stmt->setString(1, pattern);
		return result_rows + static_cast<long long>(stmt->executeUpdate());
	} catch (const sql::SQLException& e) {
		std::printf("[mysql] DeleteByUniqueIdLike failed: %s (code=%d)\n", e.what(), e.getErrorCode());
		return -1;
	}
}

bool Mysql::SnapshotFriendPair(int first_uid, int second_uid, FriendPairState& state) {
	state = FriendPairState{};
	if (!con_) return false;
	try {
		auto stmt = std::unique_ptr<sql::PreparedStatement>(con_->prepareStatement(
			"SELECT self_id, friend_id, back FROM friend WHERE "
			"(self_id = ? AND friend_id = ?) OR (self_id = ? AND friend_id = ?)"));
		stmt->setInt(1, first_uid);
		stmt->setInt(2, second_uid);
		stmt->setInt(3, second_uid);
		stmt->setInt(4, first_uid);
		auto rows = std::unique_ptr<sql::ResultSet>(stmt->executeQuery());
		while (rows->next()) {
			if (rows->getInt("self_id") == first_uid) {
				state.first_to_second = true;
				state.first_remark = rows->getString("back");
			} else {
				state.second_to_first = true;
				state.second_remark = rows->getString("back");
			}
		}
		return true;
	} catch (const sql::SQLException& e) {
		std::printf("[mysql] SnapshotFriendPair failed: %s (code=%d)\n",
			e.what(), e.getErrorCode());
		return false;
	}
}

bool Mysql::RemoveFriendPair(int first_uid, int second_uid) {
	if (!con_) return false;
	try {
		auto stmt = std::unique_ptr<sql::PreparedStatement>(con_->prepareStatement(
			"DELETE FROM friend WHERE (self_id = ? AND friend_id = ?) "
			"OR (self_id = ? AND friend_id = ?)"));
		stmt->setInt(1, first_uid);
		stmt->setInt(2, second_uid);
		stmt->setInt(3, second_uid);
		stmt->setInt(4, first_uid);
		stmt->executeUpdate();
		return true;
	} catch (const sql::SQLException& e) {
		std::printf("[mysql] RemoveFriendPair failed: %s (code=%d)\n",
			e.what(), e.getErrorCode());
		return false;
	}
}

bool Mysql::RestoreFriendPair(int first_uid, int second_uid,
	                          const FriendPairState& state) {
	if (!con_) return false;
	try {
		con_->setAutoCommit(false);
		auto clear = std::unique_ptr<sql::PreparedStatement>(con_->prepareStatement(
			"DELETE FROM friend WHERE (self_id = ? AND friend_id = ?) "
			"OR (self_id = ? AND friend_id = ?)"));
		clear->setInt(1, first_uid);
		clear->setInt(2, second_uid);
		clear->setInt(3, second_uid);
		clear->setInt(4, first_uid);
		clear->executeUpdate();

		auto insert = std::unique_ptr<sql::PreparedStatement>(con_->prepareStatement(
			"INSERT INTO friend(self_id, friend_id, back) VALUES(?, ?, ?)"));
		if (state.first_to_second) {
			insert->setInt(1, first_uid);
			insert->setInt(2, second_uid);
			insert->setString(3, state.first_remark);
			insert->executeUpdate();
		}
		if (state.second_to_first) {
			insert->setInt(1, second_uid);
			insert->setInt(2, first_uid);
			insert->setString(3, state.second_remark);
			insert->executeUpdate();
		}
		con_->commit();
		con_->setAutoCommit(true);
		return true;
	} catch (const sql::SQLException& e) {
		try {
			con_->rollback();
			con_->setAutoCommit(true);
		} catch (...) {
		}
		std::printf("[mysql] RestoreFriendPair failed: %s (code=%d)\n",
			e.what(), e.getErrorCode());
		return false;
	}
}

long long Mysql::CountFriendDirections(int first_uid, int second_uid) {
	if (!con_) return -1;
	try {
		auto stmt = std::unique_ptr<sql::PreparedStatement>(con_->prepareStatement(
			"SELECT COUNT(*) AS c FROM friend WHERE "
			"(self_id = ? AND friend_id = ?) OR (self_id = ? AND friend_id = ?)"));
		stmt->setInt(1, first_uid);
		stmt->setInt(2, second_uid);
		stmt->setInt(3, second_uid);
		stmt->setInt(4, first_uid);
		auto rows = std::unique_ptr<sql::ResultSet>(stmt->executeQuery());
		return rows->next() ? rows->getInt64("c") : 0;
	} catch (const sql::SQLException& e) {
		std::printf("[mysql] CountFriendDirections failed: %s (code=%d)\n",
			e.what(), e.getErrorCode());
		return -1;
	}
}

std::vector<RecvRow> Mysql::QueryRecvRows(std::int64_t uid,
	                                     std::uint64_t after_recv_seq,
	                                     int limit) {
	std::vector<RecvRow> out;
	if (!con_) return out;
	try {
		std::string sql =
			"SELECT recv_seq, message_id FROM chat_message "
			"WHERE recv_id = ? AND recv_seq > ? ORDER BY recv_seq ASC";
		if (limit > 0) sql += " LIMIT " + std::to_string(limit);
		auto stmt = std::unique_ptr<sql::PreparedStatement>(con_->prepareStatement(sql));
		stmt->setInt64(1, uid);
		stmt->setUInt64(2, after_recv_seq);
		auto rows = std::unique_ptr<sql::ResultSet>(stmt->executeQuery());
		while (rows->next()) {
			RecvRow row;
			row.recv_seq = static_cast<std::uint64_t>(rows->getInt64("recv_seq"));
			row.message_id = static_cast<std::uint64_t>(rows->getInt64("message_id"));
			out.push_back(row);
		}
	} catch (const sql::SQLException& e) {
		std::printf("[mysql] QueryRecvRows failed: %s (code=%d)\n", e.what(), e.getErrorCode());
	}
	return out;
}

std::uint64_t Mysql::LastRecvSeq(std::int64_t uid) {
	if (!con_) return 0;
	try {
		auto stmt = std::unique_ptr<sql::PreparedStatement>(con_->prepareStatement(
			"SELECT last_recv_seq FROM user WHERE uid = ?"));
		stmt->setInt64(1, uid);
		auto rows = std::unique_ptr<sql::ResultSet>(stmt->executeQuery());
		return rows->next()
			? static_cast<std::uint64_t>(rows->getInt64("last_recv_seq")) : 0;
	} catch (const sql::SQLException& e) {
		std::printf("[mysql] LastRecvSeq failed: %s (code=%d)\n", e.what(), e.getErrorCode());
		return 0;
	}
}

long long Mysql::GetChatMessageAutoIncrement() {
	if (!con_) return -1;
	try {
		auto stmt = std::unique_ptr<sql::PreparedStatement>(con_->prepareStatement(
			"SELECT AUTO_INCREMENT FROM information_schema.TABLES "
			"WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'chat_message'"));
		auto rows = std::unique_ptr<sql::ResultSet>(stmt->executeQuery());
		return rows->next() ? rows->getInt64("AUTO_INCREMENT") : -1;
	} catch (const sql::SQLException& e) {
		std::printf("[mysql] GetChatMessageAutoIncrement failed: %s (code=%d)\n",
			e.what(), e.getErrorCode());
		return -1;
	}
}

bool Mysql::SetChatMessageAutoIncrement(std::int64_t value) {
	if (!con_) return false;
	try {
		auto stmt = std::unique_ptr<sql::Statement>(con_->createStatement());
		stmt->execute("ALTER TABLE chat_message AUTO_INCREMENT = " + std::to_string(value));
		return true;
	} catch (const sql::SQLException& e) {
		std::printf("[mysql] SetChatMessageAutoIncrement failed: %s (code=%d)\n",
			e.what(), e.getErrorCode());
		return false;
	}
}

bool Mysql::BackdateMessageUpdatedAt(std::int64_t message_id, int days_ago) {
	if (!con_) return false;
	try {
		auto stmt = std::unique_ptr<sql::PreparedStatement>(con_->prepareStatement(
			"UPDATE chat_message SET updated_at = "
			"DATE_SUB(NOW(), INTERVAL ? DAY) WHERE message_id = ?"));
		stmt->setInt(1, days_ago);
		stmt->setInt64(2, message_id);
		stmt->executeUpdate();
		return true;
	} catch (const sql::SQLException& e) {
		std::printf("[mysql] BackdateMessageUpdatedAt failed: %s (code=%d)\n",
			e.what(), e.getErrorCode());
		return false;
	}
}

} // namespace imt
