#include "MysqlDao.h"

#include "ConfigMgr.h"
#include "Defer.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {

const char* kResourceMessageProjection =
	"m.message_id,m.thread_id,m.sender_user_id,"
	"CASE WHEN pc.lower_user_id=m.sender_user_id THEN pc.higher_user_id "
	"ELSE pc.lower_user_id END AS recipient_user_id,"
	"m.client_message_id,m.message_type,m.status,m.created_at,"
	"r.original_file_name,r.file_size_bytes,r.sha256,r.mime_type,"
	"COALESCE(e.event_seq,0) AS event_seq ";

/// MySQL 连接池默认/上下限（[Mysql] PoolSize 可覆盖默认值）
constexpr int kDefaultMySqlPoolSize = 12;
constexpr int kMaxMySqlPoolSize = 128;
/// InnoDB 死锁（1213/40001）的有界重试次数与退避间隔
constexpr int kPublishDeadlockMaxRetries = 3;
constexpr auto kPublishDeadlockBackoff = std::chrono::milliseconds(20);

} // namespace

MysqlDao::MysqlDao() {
	auto& cfg = ConfigMgr::Inst();
	int pool_size = kDefaultMySqlPoolSize;
	const std::string size_str = cfg["Mysql"]["PoolSize"];
	if (!size_str.empty()) {
		const int parsed = std::atoi(size_str.c_str());
		if (parsed > 0) {
			pool_size = (parsed > kMaxMySqlPoolSize) ? kMaxMySqlPoolSize : parsed;
		}
	}
	pool_.reset(new MySqlPool(cfg["Mysql"]["Host"] + ":" + cfg["Mysql"]["Port"],
		cfg["Mysql"]["User"], cfg["Mysql"]["Passwd"], cfg["Mysql"]["Schema"], pool_size));
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

namespace {

/// InnoDB 死锁：MySQL 错误码 1213（ER_LOCK_DEADLOCK）或 ANSI SQLState 40001。
bool IsDeadlock(const sql::SQLException& error) {
	return error.getErrorCode() == 1213
		|| std::string(error.getSQLState()) == "40001";
}

} // namespace

MysqlDao::PublishResult MysqlDao::CompleteResourceUpload(long long message_id,
	int sender_user_id, unsigned long long& event_seq) {
	event_seq = 0;
	for (int attempt = 1; ; ++attempt) {
		bool deadlock = false;
		const PublishResult rc = CompleteResourceUploadOnce(
			message_id, sender_user_id, event_seq, deadlock);
		if (rc != PublishResult::Transient || !deadlock
			|| attempt >= kPublishDeadlockMaxRetries) {
			return rc;
		}
		std::cerr << "CompleteResourceUpload deadlock msg=" << message_id
			<< ", retry " << attempt << "/" << (kPublishDeadlockMaxRetries - 1)
			<< std::endl;
		std::this_thread::sleep_for(kPublishDeadlockBackoff);
	}
}

MysqlDao::PublishResult MysqlDao::CompleteResourceUploadOnce(long long message_id,
	int sender_user_id, unsigned long long& event_seq, bool& deadlock) {
	deadlock = false;
	event_seq = 0;
	auto connection = pool_->getConnection();
	if (!connection) return PublishResult::Transient;
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
			return PublishResult::NotFound;
		}
		const int message_type = row->getInt("message_type");
		const int status = row->getInt("status");
		const int recipient_user_id = row->getInt("recipient_user_id");
		if ((message_type != 1 && message_type != 3) || recipient_user_id <= 0) {
			finish(false);
			return PublishResult::NotFound;
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
				//Published 却无事件行：数据不一致，重试无益，按永久错误处理
				finish(false);
				return PublishResult::NotFound;
			}
			event_seq = existing->getUInt64("event_seq");
			finish(true);
			return PublishResult::AlreadyPublished;
		}
		if (status != static_cast<int>(MessageStatus::Pending)) {
			//清理任务已置 Failed 等终态：不再接受发布
			finish(false);
			return PublishResult::NotPending;
		}

		auto read_head = std::unique_ptr<sql::PreparedStatement>(db->prepareStatement(
			"SELECT last_event_seq FROM users WHERE user_id=? FOR UPDATE"));
		read_head->setInt(1, recipient_user_id);
		auto head = std::unique_ptr<sql::ResultSet>(read_head->executeQuery());
		if (!head->next()) {
			finish(false);
			return PublishResult::NotFound;
		}
		event_seq = head->getUInt64("last_event_seq") + 1;

		auto update_head = std::unique_ptr<sql::PreparedStatement>(db->prepareStatement(
			"UPDATE users SET last_event_seq=? WHERE user_id=?"));
		update_head->setUInt64(1, event_seq);
		update_head->setInt(2, recipient_user_id);
		if (update_head->executeUpdate() != 1) {
			finish(false);
			return PublishResult::Transient;
		}

		auto publish = std::unique_ptr<sql::PreparedStatement>(db->prepareStatement(
			"UPDATE chat_messages SET status=? WHERE message_id=? AND status=?"));
		publish->setInt(1, static_cast<int>(MessageStatus::Published));
		publish->setInt64(2, message_id);
		publish->setInt(3, static_cast<int>(MessageStatus::Pending));
		if (publish->executeUpdate() != 1) {
			finish(false);
			return PublishResult::Transient;
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
			return PublishResult::Transient;
		}

		finish(true);
		return PublishResult::Published;
	} catch (const sql::SQLException& error) {
		deadlock = IsDeadlock(error);
		try {
			db->rollback();
			db->setAutoCommit(true);
		} catch (...) {
		}
		std::cerr << "CompleteResourceUpload SQLException: " << error.what()
			<< " code=" << error.getErrorCode()
			<< " sqlstate=" << error.getSQLState() << std::endl;
		return PublishResult::Transient;
	}
}

bool MysqlDao::MarkResourceFailed(long long message_id) {
	auto connection = pool_->getConnection();
	if (!connection) return false;
	Defer defer([this, &connection]() { pool_->returnConnection(std::move(connection)); });
	try {
		auto update = std::unique_ptr<sql::PreparedStatement>(
			connection->_con->prepareStatement(
				"UPDATE chat_messages SET status=? WHERE message_id=? AND status=?"));
		update->setInt(1, static_cast<int>(MessageStatus::Failed));
		update->setInt64(2, message_id);
		update->setInt(3, static_cast<int>(MessageStatus::Pending));
		return update->executeUpdate() == 1;
	} catch (const sql::SQLException& error) {
		std::cerr << "MarkResourceFailed SQLException: " << error.what() << std::endl;
		return false;
	}
}


MysqlDao::ChatMsgLookup MysqlDao::GetChatMsgById(long long message_id,
	std::shared_ptr<ChatMessage>& out) {
	out.reset();
	auto connection = pool_->getConnection();
	if (!connection) return ChatMsgLookup::Transient;
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
		if (!row->next()) return ChatMsgLookup::NotFound;

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
		out = message;
		return ChatMsgLookup::Found;
	} catch (const sql::SQLException& error) {
		std::cerr << "GetChatMsgById SQLException: " << error.what() << std::endl;
		return ChatMsgLookup::Transient;
	}
}

bool MysqlDao::GetExpiredResourceIds(const std::string& before_time,
	const std::string& after_created_at, long long after_message_id,
	int limit, std::vector<ExpiredResource>& out) {
	out.clear();
	auto connection = pool_->getConnection();
	if (!connection) return false;
	Defer defer([this, &connection]() { pool_->returnConnection(std::move(connection)); });
	try {
		//(created_at, message_id) 游标分页。首轮（游标为空串）完全省略
		//游标条件，不依赖空串与 DATETIME 的隐式比较；后续页使用精确条件，
		//避免 LIMIT 永远取同批头部行造成后面行饿死。
		constexpr const char* kSelectHead =
			"SELECT message_id,sender_user_id,created_at FROM chat_messages "
			"WHERE status=? AND message_type IN (1,3) AND created_at<? ";
		constexpr const char* kOrderLimit =
			" ORDER BY created_at ASC, message_id ASC LIMIT ?";
		const bool first_page = after_created_at.empty();
		const std::string sql = first_page
			? std::string(kSelectHead) + kOrderLimit
			: std::string(kSelectHead)
				+ "AND (created_at>? OR (created_at=? AND message_id>?))" + kOrderLimit;
		auto query = std::unique_ptr<sql::PreparedStatement>(
			connection->_con->prepareStatement(sql));
		if (first_page) {
			query->setInt(1, static_cast<int>(MessageStatus::Pending));
			query->setString(2, before_time);
			query->setInt(3, limit);
		}
		else {
			query->setInt(1, static_cast<int>(MessageStatus::Pending));
			query->setString(2, before_time);
			query->setString(3, after_created_at);
			query->setString(4, after_created_at);
			query->setInt64(5, after_message_id);
			query->setInt(6, limit);
		}
		auto rows = std::unique_ptr<sql::ResultSet>(query->executeQuery());
		while (rows->next()) {
			ExpiredResource item;
			item.message_id = rows->getInt64("message_id");
			item.sender_user_id = rows->getInt("sender_user_id");
			item.created_at = rows->getString("created_at");
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
