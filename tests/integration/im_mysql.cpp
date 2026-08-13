// im_mysql.cpp — mysql-concpp wrapper implementation.
#include "im_mysql.h"

#include <memory>
#include <sstream>

#include <jdbc/mysql_driver.h>
#include <jdbc/mysql_connection.h>
#include <jdbc/cppconn/prepared_statement.h>
#include <jdbc/cppconn/resultset.h>
#include <jdbc/cppconn/statement.h>
#include <jdbc/cppconn/exception.h>

namespace imt {

Mysql::~Mysql() { Close(); }

bool Mysql::Connect(const std::string& host, int port, const std::string& user,
                    const std::string& pwd, const std::string& schema) {
	Close();
	try {
		sql::mysql::MySQL_Driver* drv = sql::mysql::get_mysql_driver_instance();
		const std::string url = "tcp://" + host + ":" + std::to_string(port);
		con_ = drv->connect(url, user, pwd);
		if (!con_) return false;
		con_->setSchema(schema);
		return true;
	} catch (sql::SQLException& e) {
		std::printf("[mysql] connect failed: %s (code=%d)\n", e.what(), e.getErrorCode());
		return false;
	}
}

void Mysql::Close() {
	if (con_) { delete con_; con_ = nullptr; }
}

std::vector<ChatMessageRow> Mysql::QueryByUniqueId(int sender_id, const std::string& unique_id) {
	std::vector<ChatMessageRow> out;
	if (!con_) return out;
	try {
		std::unique_ptr<sql::PreparedStatement> pstmt(con_->prepareStatement(
			"SELECT message_id, thread_id, sender_id, recv_id, unique_id, content, "
			"created_at AS chat_time, status, msg_type, content_size, delivery_status "
			"FROM chat_message WHERE sender_id = ? AND unique_id = ?"));
		pstmt->setInt(1, sender_id);
		pstmt->setString(2, unique_id);
		std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
		while (res->next()) {
			ChatMessageRow r;
			r.message_id = res->getInt64("message_id");
			r.thread_id  = res->getInt64("thread_id");
			r.sender_id  = res->getInt("sender_id");
			r.recv_id    = res->getInt("recv_id");
			r.unique_id  = res->getString("unique_id");
			r.content    = res->getString("content");
			r.chat_time  = res->getString("chat_time");
			r.status     = res->getInt("status");
			r.msg_type   = res->getInt("msg_type");
			r.content_size = static_cast<std::uint64_t>(res->getInt64("content_size"));
			r.delivery_status = res->getInt("delivery_status");
			out.push_back(std::move(r));
		}
	} catch (sql::SQLException& e) {
		std::printf("[mysql] QueryByUniqueId failed: %s (code=%d)\n", e.what(), e.getErrorCode());
	}
	return out;
}

long long Mysql::CountByUniqueId(const std::string& unique_id) {
	if (!con_) return -1;
	try {
		std::unique_ptr<sql::PreparedStatement> pstmt(con_->prepareStatement(
			"SELECT COUNT(*) AS c FROM chat_message WHERE unique_id = ?"));
		pstmt->setString(1, unique_id);
		std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
		if (res->next()) return res->getInt64("c");
		return 0;
	} catch (sql::SQLException& e) {
		std::printf("[mysql] CountByUniqueId failed: %s (code=%d)\n", e.what(), e.getErrorCode());
		return -1;
	}
}

long long Mysql::CountByUniqueIdLike(const std::string& pattern) {
	if (!con_) return -1;
	try {
		std::unique_ptr<sql::PreparedStatement> pstmt(con_->prepareStatement(
			"SELECT COUNT(*) AS c FROM chat_message WHERE unique_id LIKE ?"));
		pstmt->setString(1, pattern);
		std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
		if (res->next()) return res->getInt64("c");
		return 0;
	} catch (sql::SQLException& e) {
		std::printf("[mysql] CountByUniqueIdLike failed: %s (code=%d)\n", e.what(), e.getErrorCode());
		return -1;
	}
}

long long Mysql::CountByUniqueIdLikeAndDelivery(const std::string& pattern,
                                                   int delivery_status) {
	if (!con_) return -1;
	try {
		std::unique_ptr<sql::PreparedStatement> pstmt(con_->prepareStatement(
			"SELECT COUNT(*) AS c FROM chat_message WHERE unique_id LIKE ? AND delivery_status = ?"));
		pstmt->setString(1, pattern);
		pstmt->setInt(2, delivery_status);
		std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
		if (res->next()) return res->getInt64("c");
		return 0;
	} catch (sql::SQLException& e) {
		std::printf("[mysql] CountByUniqueIdLikeAndDelivery failed: %s (code=%d)\n", e.what(), e.getErrorCode());
		return -1;
	}
}

std::vector<ChatMessageRow> Mysql::QueryByMessageId(std::int64_t message_id) {
	std::vector<ChatMessageRow> out;
	if (!con_) return out;
	try {
		std::unique_ptr<sql::PreparedStatement> pstmt(con_->prepareStatement(
			"SELECT message_id, thread_id, sender_id, recv_id, unique_id, content, "
			"created_at AS chat_time, status, msg_type, content_size, delivery_status "
			"FROM chat_message WHERE message_id = ?"));
		pstmt->setInt64(1, message_id);
		std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
		while (res->next()) {
			ChatMessageRow r;
			r.message_id = res->getInt64("message_id");
			r.thread_id  = res->getInt64("thread_id");
			r.sender_id  = res->getInt("sender_id");
			r.recv_id    = res->getInt("recv_id");
			r.unique_id  = res->getString("unique_id");
			r.content    = res->getString("content");
			r.chat_time  = res->getString("chat_time");
			r.status     = res->getInt("status");
			r.msg_type   = res->getInt("msg_type");
			r.content_size = static_cast<std::uint64_t>(res->getInt64("content_size"));
			r.delivery_status = res->getInt("delivery_status");
			out.push_back(std::move(r));
		}
	} catch (sql::SQLException& e) {
		std::printf("[mysql] QueryByMessageId failed: %s (code=%d)\n", e.what(), e.getErrorCode());
	}
	return out;
}

std::vector<ChatMessageRow> Mysql::QueryByRecvIdAndDelivery(int recv_id,
                                                            int delivery_status) {
	std::vector<ChatMessageRow> out;
	if (!con_) return out;
	try {
		std::unique_ptr<sql::PreparedStatement> pstmt(con_->prepareStatement(
			"SELECT message_id, thread_id, sender_id, recv_id, unique_id, content, "
			"created_at AS chat_time, status, msg_type, content_size, delivery_status "
			"FROM chat_message WHERE recv_id = ? AND delivery_status = ? "
			"ORDER BY message_id"));
		pstmt->setInt(1, recv_id);
		pstmt->setInt(2, delivery_status);
		std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
		while (res->next()) {
			ChatMessageRow r;
			r.message_id = res->getInt64("message_id");
			r.thread_id  = res->getInt64("thread_id");
			r.sender_id  = res->getInt("sender_id");
			r.recv_id    = res->getInt("recv_id");
			r.unique_id  = res->getString("unique_id");
			r.content    = res->getString("content");
			r.chat_time  = res->getString("chat_time");
			r.status     = res->getInt("status");
			r.msg_type   = res->getInt("msg_type");
			r.content_size = static_cast<std::uint64_t>(res->getInt64("content_size"));
			r.delivery_status = res->getInt("delivery_status");
			out.push_back(std::move(r));
		}
	} catch (sql::SQLException& e) {
		std::printf("[mysql] QueryByRecvIdAndDelivery failed: %s (code=%d)\n", e.what(), e.getErrorCode());
	}
	return out;
}

long long Mysql::DeleteByUniqueIdLike(const std::string& pattern) {
	if (!con_) return -1;
	try {
		std::unique_ptr<sql::PreparedStatement> pstmt(con_->prepareStatement(
			"DELETE FROM chat_message WHERE unique_id LIKE ?"));
		pstmt->setString(1, pattern);
		return static_cast<long long>(pstmt->executeUpdate());
	} catch (sql::SQLException& e) {
		std::printf("[mysql] DeleteByUniqueIdLike failed: %s (code=%d)\n", e.what(), e.getErrorCode());
		return -1;
	}
}

// ---- user_message_sync 断言辅助（增量同步） ---------------------------------

std::vector<SyncRow> Mysql::QuerySyncRows(std::int64_t uid, std::uint64_t after_seq,
                                          int limit) {
	std::vector<SyncRow> out;
	if (!con_) return out;
	try {
		std::string sql =
			"SELECT sync_seq, message_id FROM user_message_sync "
			"WHERE uid = ? AND sync_seq > ? ORDER BY sync_seq ASC";
		if (limit > 0) sql += " LIMIT " + std::to_string(limit);
		std::unique_ptr<sql::PreparedStatement> pstmt(con_->prepareStatement(sql));
		pstmt->setInt64(1, uid);
		pstmt->setUInt64(2, after_seq);
		std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
		while (res->next()) {
			SyncRow r;
			r.sync_seq   = static_cast<std::uint64_t>(res->getInt64("sync_seq"));
			r.message_id = static_cast<std::uint64_t>(res->getInt64("message_id"));
			out.push_back(r);
		}
	} catch (sql::SQLException& e) {
		std::printf("[mysql] QuerySyncRows failed: %s (code=%d)\n", e.what(), e.getErrorCode());
	}
	return out;
}

std::uint64_t Mysql::MaxSyncSeq(std::int64_t uid) {
	if (!con_) return 0;
	try {
		std::unique_ptr<sql::PreparedStatement> pstmt(con_->prepareStatement(
			"SELECT COALESCE(MAX(sync_seq), 0) AS m FROM user_message_sync WHERE uid = ?"));
		pstmt->setInt64(1, uid);
		std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
		if (res->next()) return static_cast<std::uint64_t>(res->getInt64("m"));
		return 0;
	} catch (sql::SQLException& e) {
		std::printf("[mysql] MaxSyncSeq failed: %s (code=%d)\n", e.what(), e.getErrorCode());
		return 0;
	}
}

long long Mysql::CountSyncRowsByUniqueIdLike(const std::string& pattern) {
	if (!con_) return -1;
	try {
		std::unique_ptr<sql::PreparedStatement> pstmt(con_->prepareStatement(
			"SELECT COUNT(*) AS c FROM user_message_sync s "
			"JOIN chat_message m ON m.message_id = s.message_id "
			"WHERE m.unique_id LIKE ?"));
		pstmt->setString(1, pattern);
		std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
		if (res->next()) return res->getInt64("c");
		return 0;
	} catch (sql::SQLException& e) {
		std::printf("[mysql] CountSyncRowsByUniqueIdLike failed: %s (code=%d)\n", e.what(), e.getErrorCode());
		return -1;
	}
}

long long Mysql::DeleteSyncRowsByUniqueIdLike(const std::string& pattern) {
	if (!con_) return -1;
	try {
		std::unique_ptr<sql::PreparedStatement> pstmt(con_->prepareStatement(
			"DELETE s FROM user_message_sync s "
			"JOIN chat_message m ON m.message_id = s.message_id "
			"WHERE m.unique_id LIKE ?"));
		pstmt->setString(1, pattern);
		return static_cast<long long>(pstmt->executeUpdate());
	} catch (sql::SQLException& e) {
		std::printf("[mysql] DeleteSyncRowsByUniqueIdLike failed: %s (code=%d)\n", e.what(), e.getErrorCode());
		return -1;
	}
}

long long Mysql::GetChatMessageAutoIncrement() {
	if (!con_) return -1;
	try {
		std::unique_ptr<sql::PreparedStatement> pstmt(con_->prepareStatement(
			"SELECT AUTO_INCREMENT FROM information_schema.TABLES "
			"WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'chat_message'"));
		std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
		if (res->next()) return res->getInt64("AUTO_INCREMENT");
		return -1;
	} catch (sql::SQLException& e) {
		std::printf("[mysql] GetChatMessageAutoIncrement failed: %s (code=%d)\n", e.what(), e.getErrorCode());
		return -1;
	}
}

bool Mysql::SetChatMessageAutoIncrement(std::int64_t value) {
	if (!con_) return false;
	try {
		std::unique_ptr<sql::Statement> stmt(con_->createStatement());
		stmt->execute("ALTER TABLE chat_message AUTO_INCREMENT = " + std::to_string(value));
		return true;
	} catch (sql::SQLException& e) {
		std::printf("[mysql] SetChatMessageAutoIncrement failed: %s (code=%d)\n", e.what(), e.getErrorCode());
		return false;
	}
}

} // namespace imt
