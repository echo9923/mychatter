#include "MysqlDao.h"

#include "ConfigMgr.h"
#include "Defer.h"

#include <iostream>

namespace {

const char* kResourceMessageProjection =
	"m.message_id,m.thread_id,m.sender_user_id,"
	"CASE WHEN pc.lower_user_id=m.sender_user_id THEN pc.higher_user_id "
	"ELSE pc.lower_user_id END AS recipient_user_id,"
	"m.client_message_id,m.message_type,m.status,m.created_at,"
	"r.original_file_name,r.file_size_bytes,r.sha256,r.mime_type,"
	"COALESCE(e.event_seq,0) AS event_seq ";

} // namespace

MysqlDao::MysqlDao() {
	auto& cfg = ConfigMgr::Inst();
	pool_.reset(new MySqlPool(cfg["Mysql"]["Host"] + ":" + cfg["Mysql"]["Port"],
		cfg["Mysql"]["User"], cfg["Mysql"]["Passwd"], cfg["Mysql"]["Schema"], 5));
}

MysqlDao::~MysqlDao() {
	pool_->Close();
}

std::shared_ptr<UserInfo> MysqlDao::GetUser(int user_id) {
	auto connection = pool_->getConnection();
	if (!connection) return nullptr;
	Defer defer([this, &connection]() { pool_->returnConnection(std::move(connection)); });
	try {
		auto query = std::unique_ptr<sql::PreparedStatement>(connection->_con->prepareStatement(
			"SELECT user_id,username,email,nickname,profile_bio,gender,avatar_key "
			"FROM users WHERE user_id=?"));
		query->setInt(1, user_id);
		auto result = std::unique_ptr<sql::ResultSet>(query->executeQuery());
		if (!result->next()) return nullptr;
		auto user = std::make_shared<UserInfo>();
		user->user_id = result->getInt("user_id");
		user->username = result->getString("username");
		user->email = result->getString("email");
		user->nickname = result->getString("nickname");
		user->profile_bio = result->getString("profile_bio");
		user->gender = result->getInt("gender");
		user->avatar_key = result->getString("avatar_key");
		return user;
	} catch (const sql::SQLException& error) {
		std::cerr << "GetUser SQLException: " << error.what() << std::endl;
		return nullptr;
	}
}

bool MysqlDao::UpdateHeadInfo(int user_id, const std::string& avatar_key) {
	auto connection = pool_->getConnection();
	if (!connection) return false;
	Defer defer([this, &connection]() { pool_->returnConnection(std::move(connection)); });
	try {
		auto update = std::unique_ptr<sql::PreparedStatement>(connection->_con->prepareStatement(
			"UPDATE users SET avatar_key=? WHERE user_id=?"));
		update->setString(1, avatar_key);
		update->setInt(2, user_id);
		return update->executeUpdate() == 1;
	} catch (const sql::SQLException& error) {
		std::cerr << "UpdateHeadInfo SQLException: " << error.what() << std::endl;
		return false;
	}
}

bool MysqlDao::CompleteResourceUpload(long long message_id, int sender_user_id,
	unsigned long long& event_seq) {
	event_seq = 0;
	auto connection = pool_->getConnection();
	if (!connection) return false;
	Defer defer([this, &connection]() { pool_->returnConnection(std::move(connection)); });
	auto& db = connection->_con;
	auto finish = [&db](bool commit) {
		if (commit) db->commit(); else db->rollback();
		db->setAutoCommit(true);
	};
	try {
		db->setAutoCommit(false);
		auto read = std::unique_ptr<sql::PreparedStatement>(db->prepareStatement(
			"SELECT m.sender_user_id,m.message_type,m.status,"
			"CASE WHEN pc.lower_user_id=m.sender_user_id THEN pc.higher_user_id "
			"ELSE pc.lower_user_id END AS recipient_user_id "
			"FROM chat_messages m "
			"JOIN private_chats pc ON pc.thread_id=m.thread_id "
			"JOIN message_resources r ON r.message_id=m.message_id "
			"WHERE m.message_id=? FOR UPDATE"));
		read->setInt64(1, message_id);
		auto row = std::unique_ptr<sql::ResultSet>(read->executeQuery());
		if (!row->next() || row->getInt("sender_user_id") != sender_user_id) {
			finish(false);
			return false;
		}
		const int message_type = row->getInt("message_type");
		const int status = row->getInt("status");
		const int recipient_user_id = row->getInt("recipient_user_id");
		if ((message_type != 1 && message_type != 3) || recipient_user_id <= 0) {
			finish(false);
			return false;
		}

		if (status == static_cast<int>(MessageStatus::Published)) {
			auto event = std::unique_ptr<sql::PreparedStatement>(db->prepareStatement(
				"SELECT event_seq FROM user_events "
				"WHERE recipient_user_id=? AND event_type=? AND message_id=?"));
			event->setInt(1, recipient_user_id);
			event->setInt(2, message_type);
			event->setInt64(3, message_id);
			auto existing = std::unique_ptr<sql::ResultSet>(event->executeQuery());
			if (!existing->next()) {
				finish(false);
				return false;
			}
			event_seq = existing->getUInt64("event_seq");
			finish(true);
			return true;
		}
		if (status != static_cast<int>(MessageStatus::Pending)) {
			finish(false);
			return false;
		}

		auto read_head = std::unique_ptr<sql::PreparedStatement>(db->prepareStatement(
			"SELECT last_event_seq FROM users WHERE user_id=? FOR UPDATE"));
		read_head->setInt(1, recipient_user_id);
		auto head = std::unique_ptr<sql::ResultSet>(read_head->executeQuery());
		if (!head->next()) {
			finish(false);
			return false;
		}
		event_seq = head->getUInt64("last_event_seq") + 1;

		auto update_head = std::unique_ptr<sql::PreparedStatement>(db->prepareStatement(
			"UPDATE users SET last_event_seq=? WHERE user_id=?"));
		update_head->setUInt64(1, event_seq);
		update_head->setInt(2, recipient_user_id);
		if (update_head->executeUpdate() != 1) {
			finish(false);
			return false;
		}

		auto publish = std::unique_ptr<sql::PreparedStatement>(db->prepareStatement(
			"UPDATE chat_messages SET status=? WHERE message_id=? AND status=?"));
		publish->setInt(1, static_cast<int>(MessageStatus::Published));
		publish->setInt64(2, message_id);
		publish->setInt(3, static_cast<int>(MessageStatus::Pending));
		if (publish->executeUpdate() != 1) {
			finish(false);
			return false;
		}

		auto insert_event = std::unique_ptr<sql::PreparedStatement>(db->prepareStatement(
			"INSERT INTO user_events(recipient_user_id,event_seq,event_type,message_id) "
			"VALUES(?,?,?,?)"));
		insert_event->setInt(1, recipient_user_id);
		insert_event->setUInt64(2, event_seq);
		insert_event->setInt(3, message_type);
		insert_event->setInt64(4, message_id);
		if (insert_event->executeUpdate() != 1) {
			finish(false);
			return false;
		}

		finish(true);
		return true;
	} catch (const sql::SQLException& error) {
		try {
			db->rollback();
			db->setAutoCommit(true);
		} catch (...) {
		}
		std::cerr << "CompleteResourceUpload SQLException: " << error.what() << std::endl;
		return false;
	}
}

std::shared_ptr<ChatMessage> MysqlDao::GetChatMsgById(long long message_id) {
	auto connection = pool_->getConnection();
	if (!connection) return nullptr;
	Defer defer([this, &connection]() { pool_->returnConnection(std::move(connection)); });
	try {
		std::string sql = "SELECT ";
		sql += kResourceMessageProjection;
		sql +=
			"FROM chat_messages m "
			"JOIN private_chats pc ON pc.thread_id=m.thread_id "
			"JOIN message_resources r ON r.message_id=m.message_id "
			"LEFT JOIN user_events e ON e.recipient_user_id="
			"CASE WHEN pc.lower_user_id=m.sender_user_id THEN pc.higher_user_id "
			"ELSE pc.lower_user_id END "
			"AND e.event_type=m.message_type AND e.message_id=m.message_id "
			"WHERE m.message_id=?";
		auto query = std::unique_ptr<sql::PreparedStatement>(
			connection->_con->prepareStatement(sql));
		query->setInt64(1, message_id);
		auto row = std::unique_ptr<sql::ResultSet>(query->executeQuery());
		if (!row->next()) return nullptr;

		auto message = std::make_shared<ChatMessage>();
		message->message_id = row->getInt64("message_id");
		message->thread_id = row->getInt64("thread_id");
		message->sender_user_id = row->getInt("sender_user_id");
		message->recipient_user_id = row->getInt("recipient_user_id");
		message->client_message_id = row->getString("client_message_id");
		message->message_type = row->getInt("message_type");
		message->status = static_cast<MessageStatus>(row->getInt("status"));
		message->created_at = row->getString("created_at");
		message->event_seq = row->getUInt64("event_seq");
		message->resource = std::make_shared<MessageResource>();
		message->resource->message_id = message->message_id;
		message->resource->original_file_name = row->getString("original_file_name");
		message->resource->file_size_bytes = row->getUInt64("file_size_bytes");
		message->resource->sha256 = row->getString("sha256");
		message->resource->mime_type = row->getString("mime_type");
		return message;
	} catch (const sql::SQLException& error) {
		std::cerr << "GetChatMsgById SQLException: " << error.what() << std::endl;
		return nullptr;
	}
}

bool MysqlDao::GetExpiredResourceIds(const std::string& before_time, int limit,
	std::vector<ExpiredResource>& out) {
	out.clear();
	auto connection = pool_->getConnection();
	if (!connection) return false;
	Defer defer([this, &connection]() { pool_->returnConnection(std::move(connection)); });
	try {
		auto query = std::unique_ptr<sql::PreparedStatement>(connection->_con->prepareStatement(
			"SELECT message_id,sender_user_id FROM chat_messages "
			"WHERE status=? AND message_type IN (1,3) AND created_at<? "
			"ORDER BY created_at ASC LIMIT ?"));
		query->setInt(1, static_cast<int>(MessageStatus::Pending));
		query->setString(2, before_time);
		query->setInt(3, limit);
		auto rows = std::unique_ptr<sql::ResultSet>(query->executeQuery());
		while (rows->next()) {
			ExpiredResource item;
			item.message_id = rows->getInt64("message_id");
			item.sender_user_id = rows->getInt("sender_user_id");
			out.push_back(item);
		}
		return true;
	} catch (const sql::SQLException& error) {
		std::cerr << "GetExpiredResourceIds SQLException: " << error.what() << std::endl;
		out.clear();
		return false;
	}
}

bool MysqlDao::MarkResourceExpired(const std::vector<ExpiredResource>& items) {
	if (items.empty()) return true;
	auto connection = pool_->getConnection();
	if (!connection) return false;
	Defer defer([this, &connection]() { pool_->returnConnection(std::move(connection)); });
	auto& db = connection->_con;
	try {
		db->setAutoCommit(false);
		auto update = std::unique_ptr<sql::PreparedStatement>(db->prepareStatement(
			"UPDATE chat_messages SET status=? WHERE message_id=? AND status=?"));
		for (const auto& item : items) {
			update->setInt(1, static_cast<int>(MessageStatus::Failed));
			update->setInt64(2, item.message_id);
			update->setInt(3, static_cast<int>(MessageStatus::Pending));
			update->executeUpdate();
		}
		db->commit();
		db->setAutoCommit(true);
		return true;
	} catch (const sql::SQLException& error) {
		try {
			db->rollback();
			db->setAutoCommit(true);
		} catch (...) {
		}
		std::cerr << "MarkResourceExpired SQLException: " << error.what() << std::endl;
		return false;
	}
}
