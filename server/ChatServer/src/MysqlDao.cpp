#include "MysqlDao.h"

#include "ConfigMgr.h"
#include "Defer.h"
#include "PasswordHash.h"

#include <algorithm>
#include <iostream>

namespace {

const char* kMessageProjection =
	"m.message_id, m.thread_id, m.sender_user_id, "
	"CASE WHEN pc.lower_user_id = m.sender_user_id THEN pc.higher_user_id "
	"ELSE pc.lower_user_id END AS recipient_user_id, "
	"m.client_message_id, m.message_type, m.text_content, m.status, m.created_at, "
	"r.original_file_name, r.file_size_bytes, r.sha256, r.mime_type ";

const char* kFriendRequestColumns =
	"friend_request_id, requester_user_id, target_user_id, client_request_id, "
	"request_message, status, thread_id, created_at";

const char* kUserColumns =
	"user_id, username, email, nickname, profile_bio, gender, avatar_key";

std::int64_t LastInsertId(sql::Connection* connection) {
	auto statement = std::unique_ptr<sql::Statement>(connection->createStatement());
	auto result = std::unique_ptr<sql::ResultSet>(
		statement->executeQuery("SELECT LAST_INSERT_ID() AS id"));
	return result->next() ? result->getInt64("id") : 0;
}

} // namespace

MysqlDao::MysqlDao() {
	auto& cfg = ConfigMgr::Inst();
	pool_.reset(new MySqlPool(cfg["Mysql"]["Host"] + ":" + cfg["Mysql"]["Port"],
		cfg["Mysql"]["User"], cfg["Mysql"]["Passwd"], cfg["Mysql"]["Schema"], 5));
}

MysqlDao::~MysqlDao() {
	pool_->Close();
}

std::shared_ptr<UserInfo> MysqlDao::ReadUser(sql::ResultSet* result) {
	if (!result) return nullptr;
	auto user = std::make_shared<UserInfo>();
	user->user_id = result->getInt("user_id");
	user->username = result->getString("username");
	user->email = result->getString("email");
	user->nickname = result->getString("nickname");
	user->profile_bio = result->getString("profile_bio");
	user->gender = result->getInt("gender");
	user->avatar_key = result->getString("avatar_key");
	return user;
}

std::shared_ptr<ChatMessage> MysqlDao::ReadMessage(sql::ResultSet* result) {
	if (!result) return nullptr;
	auto message = std::make_shared<ChatMessage>();
	message->message_id = result->getInt64("message_id");
	message->thread_id = result->getInt64("thread_id");
	message->sender_user_id = result->getInt("sender_user_id");
	message->recipient_user_id = result->getInt("recipient_user_id");
	message->client_message_id = result->getString("client_message_id");
	message->message_type = result->getInt("message_type");
	message->text_content = result->isNull("text_content")
		? std::string() : result->getString("text_content");
	message->status = static_cast<MessageStatus>(result->getInt("status"));
	message->created_at = result->getString("created_at");
	if (message->message_type == static_cast<int>(ChatMsgType::PIC) ||
		message->message_type == static_cast<int>(ChatMsgType::FILE)) {
		auto resource = std::make_shared<MessageResource>();
		resource->message_id = message->message_id;
		resource->original_file_name = result->getString("original_file_name");
		resource->file_size_bytes = result->getUInt64("file_size_bytes");
		resource->sha256 = result->getString("sha256");
		resource->mime_type = result->getString("mime_type");
		message->resource = std::move(resource);
	}
	return message;
}

std::shared_ptr<FriendRequest> MysqlDao::ReadFriendRequest(sql::ResultSet* result) {
	if (!result) return nullptr;
	auto request = std::make_shared<FriendRequest>();
	request->friend_request_id = result->getInt64("friend_request_id");
	request->requester_user_id = result->getInt("requester_user_id");
	request->target_user_id = result->getInt("target_user_id");
	request->client_request_id = result->getString("client_request_id");
	request->request_message = result->getString("request_message");
	request->status = static_cast<FriendRequestStatus>(result->getInt("status"));
	request->thread_id = result->isNull("thread_id") ? 0 : result->getInt64("thread_id");
	request->created_at = result->getString("created_at");
	return request;
}

bool MysqlDao::AllocateEventSeq(sql::Connection* connection, int recipient_user_id,
	std::uint64_t& event_seq) {
	auto select = std::unique_ptr<sql::PreparedStatement>(connection->prepareStatement(
		"SELECT last_event_seq FROM users WHERE user_id = ? FOR UPDATE"));
	select->setInt(1, recipient_user_id);
	auto result = std::unique_ptr<sql::ResultSet>(select->executeQuery());
	if (!result->next()) return false;
	event_seq = result->getUInt64("last_event_seq") + 1;
	auto update = std::unique_ptr<sql::PreparedStatement>(connection->prepareStatement(
		"UPDATE users SET last_event_seq = ? WHERE user_id = ?"));
	update->setUInt64(1, event_seq);
	update->setInt(2, recipient_user_id);
	return update->executeUpdate() == 1;
}

bool MysqlDao::InsertMessageEvent(sql::Connection* connection, int recipient_user_id,
	std::uint64_t event_seq, int event_type, std::int64_t message_id) {
	auto insert = std::unique_ptr<sql::PreparedStatement>(connection->prepareStatement(
		"INSERT INTO user_events(recipient_user_id,event_seq,event_type,message_id) "
		"VALUES(?,?,?,?)"));
	insert->setInt(1, recipient_user_id);
	insert->setUInt64(2, event_seq);
	insert->setInt(3, event_type);
	insert->setInt64(4, message_id);
	return insert->executeUpdate() == 1;
}

bool MysqlDao::InsertFriendEvent(sql::Connection* connection, int recipient_user_id,
	std::uint64_t event_seq, int event_type, std::int64_t friend_request_id) {
	auto insert = std::unique_ptr<sql::PreparedStatement>(connection->prepareStatement(
		"INSERT INTO user_events(recipient_user_id,event_seq,event_type,friend_request_id) "
		"VALUES(?,?,?,?)"));
	insert->setInt(1, recipient_user_id);
	insert->setUInt64(2, event_seq);
	insert->setInt(3, event_type);
	insert->setInt64(4, friend_request_id);
	return insert->executeUpdate() == 1;
}

bool MysqlDao::ReadPrivateChatMembers(sql::Connection* connection,
	std::int64_t thread_id, int& lower_user_id, int& higher_user_id, bool for_update) {
	std::string sql =
		"SELECT lower_user_id,higher_user_id FROM private_chats WHERE thread_id = ?";
	if (for_update) sql += " FOR UPDATE";
	auto statement = std::unique_ptr<sql::PreparedStatement>(
		connection->prepareStatement(sql));
	statement->setInt64(1, thread_id);
	auto result = std::unique_ptr<sql::ResultSet>(statement->executeQuery());
	if (!result->next()) return false;
	lower_user_id = result->getInt("lower_user_id");
	higher_user_id = result->getInt("higher_user_id");
	return true;
}

int MysqlDao::RegUser(const std::string& name, const std::string& email,
	const std::string& password) {
	const std::string hash = llfc::HashPassword(password);
	if (hash.empty()) return -1;
	auto connection = pool_->getConnection();
	if (!connection) return -1;
	Defer defer([this, &connection]() { pool_->returnConnection(std::move(connection)); });
	try {
		auto insert = std::unique_ptr<sql::PreparedStatement>(connection->_con->prepareStatement(
			"INSERT INTO users(username,email,password_hash,nickname) VALUES(?,?,?,?)"));
		insert->setString(1, name);
		insert->setString(2, email);
		insert->setString(3, hash);
		insert->setString(4, name);
		if (insert->executeUpdate() != 1) return -1;
		return static_cast<int>(LastInsertId(connection->_con.get()));
	} catch (const sql::SQLException& error) {
		std::cerr << "RegUser SQLException: " << error.what() << std::endl;
		return -1;
	}
}

bool MysqlDao::CheckEmail(const std::string& name, const std::string& email) {
	auto connection = pool_->getConnection();
	if (!connection) return false;
	Defer defer([this, &connection]() { pool_->returnConnection(std::move(connection)); });
	try {
		auto query = std::unique_ptr<sql::PreparedStatement>(connection->_con->prepareStatement(
			"SELECT email FROM users WHERE username = ?"));
		query->setString(1, name);
		auto result = std::unique_ptr<sql::ResultSet>(query->executeQuery());
		return result->next() && result->getString("email") == email;
	} catch (const sql::SQLException&) {
		return false;
	}
}

bool MysqlDao::UpdatePwd(const std::string& name, const std::string& new_password) {
	const std::string hash = llfc::HashPassword(new_password);
	if (hash.empty()) return false;
	auto connection = pool_->getConnection();
	if (!connection) return false;
	Defer defer([this, &connection]() { pool_->returnConnection(std::move(connection)); });
	try {
		auto update = std::unique_ptr<sql::PreparedStatement>(connection->_con->prepareStatement(
			"UPDATE users SET password_hash = ? WHERE username = ?"));
		update->setString(1, hash);
		update->setString(2, name);
		return update->executeUpdate() == 1;
	} catch (const sql::SQLException&) {
		return false;
	}
}

bool MysqlDao::CheckPwd(const std::string& name, const std::string& password,
	UserInfo& user_info) {
	auto connection = pool_->getConnection();
	if (!connection) return false;
	Defer defer([this, &connection]() { pool_->returnConnection(std::move(connection)); });
	try {
		auto query = std::unique_ptr<sql::PreparedStatement>(connection->_con->prepareStatement(
			"SELECT user_id,username,email,nickname,profile_bio,gender,avatar_key,password_hash "
			"FROM users WHERE username = ?"));
		query->setString(1, name);
		auto result = std::unique_ptr<sql::ResultSet>(query->executeQuery());
		if (!result->next() ||
			!llfc::VerifyPassword(password, result->getString("password_hash"))) return false;
		user_info = *ReadUser(result.get());
		return true;
	} catch (const sql::SQLException&) {
		return false;
	}
}

std::shared_ptr<UserInfo> MysqlDao::GetUser(int user_id) {
	auto connection = pool_->getConnection();
	if (!connection) return nullptr;
	Defer defer([this, &connection]() { pool_->returnConnection(std::move(connection)); });
	try {
		auto query = std::unique_ptr<sql::PreparedStatement>(connection->_con->prepareStatement(
			(std::string("SELECT ") + kUserColumns + " FROM users WHERE user_id = ?").c_str()));
		query->setInt(1, user_id);
		auto result = std::unique_ptr<sql::ResultSet>(query->executeQuery());
		return result->next() ? ReadUser(result.get()) : nullptr;
	} catch (const sql::SQLException&) {
		return nullptr;
	}
}

std::shared_ptr<UserInfo> MysqlDao::GetUser(const std::string& username) {
	auto connection = pool_->getConnection();
	if (!connection) return nullptr;
	Defer defer([this, &connection]() { pool_->returnConnection(std::move(connection)); });
	try {
		auto query = std::unique_ptr<sql::PreparedStatement>(connection->_con->prepareStatement(
			(std::string("SELECT ") + kUserColumns + " FROM users WHERE username = ?").c_str()));
		query->setString(1, username);
		auto result = std::unique_ptr<sql::ResultSet>(query->executeQuery());
		return result->next() ? ReadUser(result.get()) : nullptr;
	} catch (const sql::SQLException&) {
		return nullptr;
	}
}

FriendOperationResult MysqlDao::AddFriendApply(int requester_user_id,
	int target_user_id, const std::string& request_message,
	const std::string& client_request_id, std::shared_ptr<FriendRequest>& request) {
	request.reset();
	if (requester_user_id <= 0 || target_user_id <= 0 ||
		requester_user_id == target_user_id || client_request_id.empty()) {
		return FriendOperationResult::Failed;
	}
	auto connection = pool_->getConnection();
	if (!connection) return FriendOperationResult::Failed;
	Defer defer([this, &connection]() { pool_->returnConnection(std::move(connection)); });
	auto* sql_connection = connection->_con.get();
	try {
		sql_connection->setAutoCommit(false);
		auto lock_target = std::unique_ptr<sql::PreparedStatement>(sql_connection->prepareStatement(
			"SELECT user_id FROM users WHERE user_id = ? FOR UPDATE"));
		lock_target->setInt(1, target_user_id);
		auto target = std::unique_ptr<sql::ResultSet>(lock_target->executeQuery());
		if (!target->next()) {
			sql_connection->rollback();
			return FriendOperationResult::NotFound;
		}
		auto check_requester = std::unique_ptr<sql::PreparedStatement>(sql_connection->prepareStatement(
			"SELECT user_id FROM users WHERE user_id = ?"));
		check_requester->setInt(1, requester_user_id);
		auto requester = std::unique_ptr<sql::ResultSet>(check_requester->executeQuery());
		if (!requester->next()) {
			sql_connection->rollback();
			return FriendOperationResult::NotFound;
		}

		const int lower = std::min(requester_user_id, target_user_id);
		const int higher = std::max(requester_user_id, target_user_id);
		auto friendship = std::unique_ptr<sql::PreparedStatement>(sql_connection->prepareStatement(
			"SELECT 1 FROM friendships WHERE lower_user_id = ? AND higher_user_id = ?"));
		friendship->setInt(1, lower);
		friendship->setInt(2, higher);
		auto friendship_result = std::unique_ptr<sql::ResultSet>(friendship->executeQuery());
		if (friendship_result->next()) {
			sql_connection->rollback();
			return FriendOperationResult::AlreadyFriends;
		}

		auto by_key = std::unique_ptr<sql::PreparedStatement>(sql_connection->prepareStatement(
			(std::string("SELECT ") + kFriendRequestColumns +
			 " FROM friend_requests WHERE requester_user_id = ? AND client_request_id = ? FOR UPDATE").c_str()));
		by_key->setInt(1, requester_user_id);
		by_key->setString(2, client_request_id);
		auto key_result = std::unique_ptr<sql::ResultSet>(by_key->executeQuery());
		if (key_result->next()) {
			request = ReadFriendRequest(key_result.get());
			const bool same = request->target_user_id == target_user_id &&
				request->request_message == request_message;
			sql_connection->commit();
			return same ? FriendOperationResult::Duplicate : FriendOperationResult::Conflict;
		}

		auto pending = std::unique_ptr<sql::PreparedStatement>(sql_connection->prepareStatement(
			(std::string("SELECT ") + kFriendRequestColumns +
			 " FROM friend_requests WHERE requester_user_id = ? AND target_user_id = ? "
			 "AND status = 0 ORDER BY friend_request_id LIMIT 1 FOR UPDATE").c_str()));
		pending->setInt(1, requester_user_id);
		pending->setInt(2, target_user_id);
		auto pending_result = std::unique_ptr<sql::ResultSet>(pending->executeQuery());
		if (pending_result->next()) {
			request = ReadFriendRequest(pending_result.get());
			sql_connection->commit();
			return FriendOperationResult::Duplicate;
		}

		auto insert = std::unique_ptr<sql::PreparedStatement>(sql_connection->prepareStatement(
			"INSERT INTO friend_requests(requester_user_id,target_user_id,client_request_id,"
			"request_message,status,created_at) VALUES(?,?,?,?,0,UTC_TIMESTAMP(3))"));
		insert->setInt(1, requester_user_id);
		insert->setInt(2, target_user_id);
		insert->setString(3, client_request_id);
		insert->setString(4, request_message);
		if (insert->executeUpdate() != 1) {
			sql_connection->rollback();
			return FriendOperationResult::Failed;
		}
		const std::int64_t request_id = LastInsertId(sql_connection);
		std::uint64_t event_seq = 0;
		if (request_id <= 0 || !AllocateEventSeq(sql_connection, target_user_id, event_seq) ||
			!InsertFriendEvent(sql_connection, target_user_id, event_seq,
				static_cast<int>(UserEventType::FRIEND_APPLY), request_id)) {
			sql_connection->rollback();
			return FriendOperationResult::Failed;
		}
		auto read = std::unique_ptr<sql::PreparedStatement>(sql_connection->prepareStatement(
			(std::string("SELECT ") + kFriendRequestColumns +
			 " FROM friend_requests WHERE friend_request_id = ?").c_str()));
		read->setInt64(1, request_id);
		auto result = std::unique_ptr<sql::ResultSet>(read->executeQuery());
		if (!result->next()) {
			sql_connection->rollback();
			return FriendOperationResult::Failed;
		}
		request = ReadFriendRequest(result.get());
		request->event_seq = event_seq;
		sql_connection->commit();
		return FriendOperationResult::Stored;
	} catch (const sql::SQLException& error) {
		std::cerr << "AddFriendApply SQLException: " << error.what() << std::endl;
		sql_connection->rollback();
		return FriendOperationResult::Failed;
	}
}

FriendOperationResult MysqlDao::HandleFriendApply(int handler_user_id,
	std::int64_t friend_request_id, bool accept, FriendHandleOutput& output) {
	output = FriendHandleOutput();
	auto connection = pool_->getConnection();
	if (!connection) return FriendOperationResult::Failed;
	Defer defer([this, &connection]() { pool_->returnConnection(std::move(connection)); });
	auto* sql_connection = connection->_con.get();
	try {
		sql_connection->setAutoCommit(false);
		auto read = std::unique_ptr<sql::PreparedStatement>(sql_connection->prepareStatement(
			(std::string("SELECT ") + kFriendRequestColumns +
			 " FROM friend_requests WHERE friend_request_id = ? FOR UPDATE").c_str()));
		read->setInt64(1, friend_request_id);
		auto result = std::unique_ptr<sql::ResultSet>(read->executeQuery());
		if (!result->next()) {
			sql_connection->rollback();
			return FriendOperationResult::NotFound;
		}
		output.request = ReadFriendRequest(result.get());
		if (output.request->target_user_id != handler_user_id) {
			sql_connection->rollback();
			return FriendOperationResult::Forbidden;
		}
		output.peer_user_id = output.request->requester_user_id;
		const FriendRequestStatus desired = accept
			? FriendRequestStatus::Accepted : FriendRequestStatus::Rejected;
		if (output.request->status != FriendRequestStatus::Pending) {
			const bool same = output.request->status == desired;
			output.thread_id = output.request->thread_id;
			sql_connection->commit();
			return same ? FriendOperationResult::Duplicate
				: FriendOperationResult::AlreadyHandled;
		}

		if (accept) {
			const int lower = std::min(output.request->requester_user_id,
				output.request->target_user_id);
			const int higher = std::max(output.request->requester_user_id,
				output.request->target_user_id);
			auto chat = std::unique_ptr<sql::PreparedStatement>(sql_connection->prepareStatement(
				"INSERT INTO private_chats(lower_user_id,higher_user_id) VALUES(?,?) "
				"ON DUPLICATE KEY UPDATE thread_id=LAST_INSERT_ID(thread_id)"));
			chat->setInt(1, lower);
			chat->setInt(2, higher);
			chat->executeUpdate();
			output.thread_id = LastInsertId(sql_connection);
			if (output.thread_id <= 0) {
				sql_connection->rollback();
				return FriendOperationResult::Failed;
			}
			auto friendship = std::unique_ptr<sql::PreparedStatement>(sql_connection->prepareStatement(
				"INSERT IGNORE INTO friendships(lower_user_id,higher_user_id) VALUES(?,?)"));
			friendship->setInt(1, lower);
			friendship->setInt(2, higher);
			friendship->executeUpdate();
			auto update = std::unique_ptr<sql::PreparedStatement>(sql_connection->prepareStatement(
				"UPDATE friend_requests SET status=1,thread_id=? WHERE status=0 AND "
				"((requester_user_id=? AND target_user_id=?) OR "
				"(requester_user_id=? AND target_user_id=?))"));
			update->setInt64(1, output.thread_id);
			update->setInt(2, lower);
			update->setInt(3, higher);
			update->setInt(4, higher);
			update->setInt(5, lower);
			update->executeUpdate();
			output.request->status = FriendRequestStatus::Accepted;
			output.request->thread_id = output.thread_id;
		} else {
			auto update = std::unique_ptr<sql::PreparedStatement>(sql_connection->prepareStatement(
				"UPDATE friend_requests SET status=2 WHERE friend_request_id=? AND status=0"));
			update->setInt64(1, friend_request_id);
			if (update->executeUpdate() != 1) {
				sql_connection->rollback();
				return FriendOperationResult::AlreadyHandled;
			}
			output.request->status = FriendRequestStatus::Rejected;
		}

		std::uint64_t event_seq = 0;
		const int event_type = accept ? static_cast<int>(UserEventType::FRIEND_ACCEPT)
			: static_cast<int>(UserEventType::FRIEND_REJECT);
		if (!AllocateEventSeq(sql_connection, output.request->requester_user_id, event_seq) ||
			!InsertFriendEvent(sql_connection, output.request->requester_user_id,
				event_seq, event_type, friend_request_id)) {
			sql_connection->rollback();
			return FriendOperationResult::Failed;
		}
		output.request->event_seq = event_seq;
		sql_connection->commit();
		return FriendOperationResult::Stored;
	} catch (const sql::SQLException& error) {
		std::cerr << "HandleFriendApply SQLException: " << error.what() << std::endl;
		sql_connection->rollback();
		return FriendOperationResult::Failed;
	}
}

bool MysqlDao::GetApplyList(int target_user_id,
	std::vector<std::shared_ptr<ApplyInfo>>& requests,
	std::int64_t after_friend_request_id, int limit) {
	requests.clear();
	auto connection = pool_->getConnection();
	if (!connection) return false;
	Defer defer([this, &connection]() { pool_->returnConnection(std::move(connection)); });
	try {
		auto query = std::unique_ptr<sql::PreparedStatement>(connection->_con->prepareStatement(
			"SELECT fr.friend_request_id,fr.requester_user_id,fr.target_user_id,"
			"fr.client_request_id,fr.request_message,fr.status,fr.thread_id,fr.created_at,"
			"u.user_id,u.username,u.email,u.nickname,u.profile_bio,u.gender,u.avatar_key "
			"FROM friend_requests fr JOIN users u ON u.user_id=fr.requester_user_id "
			"WHERE fr.target_user_id=? AND fr.status=0 AND fr.friend_request_id>? "
			"ORDER BY fr.friend_request_id ASC LIMIT ?"));
		query->setInt(1, target_user_id);
		query->setInt64(2, after_friend_request_id);
		query->setInt(3, limit);
		auto result = std::unique_ptr<sql::ResultSet>(query->executeQuery());
		while (result->next()) {
			auto info = std::make_shared<ApplyInfo>();
			info->request = ReadFriendRequest(result.get());
			info->peer = ReadUser(result.get());
			requests.push_back(std::move(info));
		}
		return true;
	} catch (const sql::SQLException& error) {
		std::cerr << "GetApplyList SQLException: " << error.what() << std::endl;
		return false;
	}
}

bool MysqlDao::GetFriendList(int user_id, std::vector<ContactInfo>& contacts) {
	contacts.clear();
	auto connection = pool_->getConnection();
	if (!connection) return false;
	Defer defer([this, &connection]() { pool_->returnConnection(std::move(connection)); });
	try {
		auto query = std::unique_ptr<sql::PreparedStatement>(connection->_con->prepareStatement(
			"SELECT u.user_id,u.username,u.email,u.nickname,u.profile_bio,u.gender,u.avatar_key,"
			"pc.thread_id FROM friendships f "
			"JOIN users u ON u.user_id=CASE WHEN f.lower_user_id=? THEN f.higher_user_id "
			"ELSE f.lower_user_id END "
			"JOIN private_chats pc ON pc.lower_user_id=f.lower_user_id "
			"AND pc.higher_user_id=f.higher_user_id "
			"WHERE f.lower_user_id=? OR f.higher_user_id=? ORDER BY u.user_id"));
		query->setInt(1, user_id);
		query->setInt(2, user_id);
		query->setInt(3, user_id);
		auto result = std::unique_ptr<sql::ResultSet>(query->executeQuery());
		while (result->next()) {
			ContactInfo contact;
			contact.user = ReadUser(result.get());
			contact.thread_id = result->getInt64("thread_id");
			contacts.push_back(std::move(contact));
		}
		return true;
	} catch (const sql::SQLException& error) {
		std::cerr << "GetFriendList SQLException: " << error.what() << std::endl;
		return false;
	}
}

bool MysqlDao::GetUserThreads(std::int64_t user_id, std::int64_t after_thread_id,
	int page_size, std::vector<std::shared_ptr<ChatThreadInfo>>& threads,
	bool& load_more, std::int64_t& next_thread_id) {
	threads.clear();
	load_more = false;
	next_thread_id = after_thread_id;
	auto connection = pool_->getConnection();
	if (!connection) return false;
	Defer defer([this, &connection]() { pool_->returnConnection(std::move(connection)); });
	try {
		auto query = std::unique_ptr<sql::PreparedStatement>(connection->_con->prepareStatement(
			"SELECT pc.thread_id,pc.lower_user_id,pc.higher_user_id,"
			"COALESCE(MAX(m.message_id),0) AS last_message_id "
			"FROM private_chats pc LEFT JOIN chat_messages m ON m.thread_id=pc.thread_id "
			"AND m.status=1 WHERE (pc.lower_user_id=? OR pc.higher_user_id=?) "
			"AND (?=0 OR pc.thread_id<?) GROUP BY pc.thread_id,pc.lower_user_id,pc.higher_user_id "
			"ORDER BY pc.thread_id DESC LIMIT ?"));
		query->setInt64(1, user_id);
		query->setInt64(2, user_id);
		query->setInt64(3, after_thread_id);
		query->setInt64(4, after_thread_id);
		query->setInt(5, page_size + 1);
		auto result = std::unique_ptr<sql::ResultSet>(query->executeQuery());
		while (result->next()) {
			auto thread = std::make_shared<ChatThreadInfo>();
			thread->_thread_id = result->getInt64("thread_id");
			thread->_lower_user_id = result->getInt("lower_user_id");
			thread->_higher_user_id = result->getInt("higher_user_id");
			thread->_last_msg_id = result->getInt64("last_message_id");
			threads.push_back(std::move(thread));
		}
		if (static_cast<int>(threads.size()) > page_size) {
			threads.pop_back();
			load_more = true;
		}
		if (!threads.empty()) next_thread_id = threads.back()->_thread_id;
		return true;
	} catch (const sql::SQLException& error) {
		std::cerr << "GetUserThreads SQLException: " << error.what() << std::endl;
		return false;
	}
}

bool MysqlDao::CreatePrivateChat(int requester_user_id, int target_user_id,
	std::int64_t& thread_id) {
	thread_id = 0;
	if (requester_user_id <= 0 || target_user_id <= 0 ||
		requester_user_id == target_user_id) return false;
	const int lower = std::min(requester_user_id, target_user_id);
	const int higher = std::max(requester_user_id, target_user_id);
	auto connection = pool_->getConnection();
	if (!connection) return false;
	Defer defer([this, &connection]() { pool_->returnConnection(std::move(connection)); });
	try {
		auto insert = std::unique_ptr<sql::PreparedStatement>(connection->_con->prepareStatement(
			"INSERT INTO private_chats(lower_user_id,higher_user_id) VALUES(?,?) "
			"ON DUPLICATE KEY UPDATE thread_id=LAST_INSERT_ID(thread_id)"));
		insert->setInt(1, lower);
		insert->setInt(2, higher);
		insert->executeUpdate();
		thread_id = LastInsertId(connection->_con.get());
		return thread_id > 0;
	} catch (const sql::SQLException& error) {
		std::cerr << "CreatePrivateChat SQLException: " << error.what() << std::endl;
		return false;
	}
}

bool MysqlDao::GetPrivateChatMembers(std::int64_t thread_id, int& lower_user_id,
	int& higher_user_id) {
	auto connection = pool_->getConnection();
	if (!connection) return false;
	Defer defer([this, &connection]() { pool_->returnConnection(std::move(connection)); });
	try {
		return ReadPrivateChatMembers(connection->_con.get(), thread_id,
			lower_user_id, higher_user_id);
	} catch (const sql::SQLException&) {
		return false;
	}
}

SaveMessageResult MysqlDao::AddChatMsg(const std::shared_ptr<ChatMessage>& message) {
	if (!message || message->thread_id <= 0 || message->sender_user_id <= 0 ||
		message->client_message_id.empty()) return SaveMessageResult::Failed;
	auto connection = pool_->getConnection();
	if (!connection) return SaveMessageResult::Failed;
	Defer defer([this, &connection]() { pool_->returnConnection(std::move(connection)); });
	auto* sql_connection = connection->_con.get();
	try {
		sql_connection->setAutoCommit(false);
		int lower = 0;
		int higher = 0;
		if (!ReadPrivateChatMembers(sql_connection, message->thread_id, lower, higher, true) ||
			(message->sender_user_id != lower && message->sender_user_id != higher)) {
			sql_connection->rollback();
			return SaveMessageResult::Failed;
		}
		message->recipient_user_id = message->sender_user_id == lower ? higher : lower;

		auto existing = std::unique_ptr<sql::PreparedStatement>(sql_connection->prepareStatement(
			(std::string("SELECT ") + kMessageProjection +
			 "FROM chat_messages m JOIN private_chats pc ON pc.thread_id=m.thread_id "
			 "LEFT JOIN message_resources r ON r.message_id=m.message_id "
			 "WHERE m.sender_user_id=? AND m.client_message_id=? FOR UPDATE").c_str()));
		existing->setInt(1, message->sender_user_id);
		existing->setString(2, message->client_message_id);
		auto existing_result = std::unique_ptr<sql::ResultSet>(existing->executeQuery());
		if (existing_result->next()) {
			auto stored = ReadMessage(existing_result.get());
			bool same = stored->thread_id == message->thread_id &&
				stored->sender_user_id == message->sender_user_id &&
				stored->message_type == message->message_type &&
				stored->text_content == message->text_content;
			if (same && stored->resource && message->resource) {
				same = stored->resource->original_file_name == message->resource->original_file_name &&
					stored->resource->file_size_bytes == message->resource->file_size_bytes &&
					stored->resource->sha256 == message->resource->sha256 &&
					stored->resource->mime_type == message->resource->mime_type;
			} else if (static_cast<bool>(stored->resource) != static_cast<bool>(message->resource)) {
				same = false;
			}
			if (!same) {
				sql_connection->rollback();
				return SaveMessageResult::Conflict;
			}
			*message = *stored;
			if (message->status == MessageStatus::Published) {
				auto event = std::unique_ptr<sql::PreparedStatement>(sql_connection->prepareStatement(
					"SELECT event_seq FROM user_events WHERE recipient_user_id=? "
					"AND event_type=? AND message_id=?"));
				event->setInt(1, message->recipient_user_id);
				event->setInt(2, message->message_type);
				event->setInt64(3, message->message_id);
				auto event_result = std::unique_ptr<sql::ResultSet>(event->executeQuery());
				if (event_result->next()) message->event_seq = event_result->getUInt64("event_seq");
			}
			sql_connection->commit();
			return SaveMessageResult::Duplicate;
		}

		const bool resource_message =
			message->message_type == static_cast<int>(ChatMsgType::PIC) ||
			message->message_type == static_cast<int>(ChatMsgType::FILE);
		if ((resource_message && !message->resource) ||
			(!resource_message && message->message_type != static_cast<int>(ChatMsgType::TEXT))) {
			sql_connection->rollback();
			return SaveMessageResult::Failed;
		}
		message->status = resource_message ? MessageStatus::Pending : MessageStatus::Published;
		auto insert = std::unique_ptr<sql::PreparedStatement>(sql_connection->prepareStatement(
			"INSERT INTO chat_messages(thread_id,sender_user_id,client_message_id,message_type,"
			"text_content,status,created_at) VALUES(?,?,?,?,?,?,?)"));
		insert->setInt64(1, message->thread_id);
		insert->setInt(2, message->sender_user_id);
		insert->setString(3, message->client_message_id);
		insert->setInt(4, message->message_type);
		if (resource_message) insert->setNull(5, sql::DataType::LONGVARCHAR);
		else insert->setString(5, message->text_content);
		insert->setInt(6, static_cast<int>(message->status));
		insert->setString(7, message->created_at);
		if (insert->executeUpdate() != 1) {
			sql_connection->rollback();
			return SaveMessageResult::Failed;
		}
		message->message_id = LastInsertId(sql_connection);
		if (resource_message) {
			message->resource->message_id = message->message_id;
			auto resource = std::unique_ptr<sql::PreparedStatement>(sql_connection->prepareStatement(
				"INSERT INTO message_resources(message_id,original_file_name,file_size_bytes,sha256,mime_type) "
				"VALUES(?,?,?,?,?)"));
			resource->setInt64(1, message->message_id);
			resource->setString(2, message->resource->original_file_name);
			resource->setUInt64(3, message->resource->file_size_bytes);
			resource->setString(4, message->resource->sha256);
			resource->setString(5, message->resource->mime_type);
			if (resource->executeUpdate() != 1) {
				sql_connection->rollback();
				return SaveMessageResult::Failed;
			}
		} else if (!AllocateEventSeq(sql_connection, message->recipient_user_id,
			message->event_seq) || !InsertMessageEvent(sql_connection,
				message->recipient_user_id, message->event_seq, message->message_type,
				message->message_id)) {
			sql_connection->rollback();
			return SaveMessageResult::Failed;
		}
		sql_connection->commit();
		return SaveMessageResult::Stored;
	} catch (const sql::SQLException& error) {
		std::cerr << "AddChatMsg SQLException: " << error.what() << std::endl;
		sql_connection->rollback();
		return SaveMessageResult::Failed;
	}
}

std::shared_ptr<PageResult> MysqlDao::LoadChatMsg(int requester_user_id,
	std::int64_t thread_id, std::int64_t before_message_id, int page_size) {
	auto connection = pool_->getConnection();
	if (!connection) return nullptr;
	Defer defer([this, &connection]() { pool_->returnConnection(std::move(connection)); });
	try {
		int lower = 0;
		int higher = 0;
		if (!ReadPrivateChatMembers(connection->_con.get(), thread_id, lower, higher) ||
			(requester_user_id != lower && requester_user_id != higher)) return nullptr;
		auto query = std::unique_ptr<sql::PreparedStatement>(connection->_con->prepareStatement(
			(std::string("SELECT ") + kMessageProjection +
			 "FROM chat_messages m JOIN private_chats pc ON pc.thread_id=m.thread_id "
			 "LEFT JOIN message_resources r ON r.message_id=m.message_id "
			 "WHERE m.thread_id=? AND m.status=1 AND (?=0 OR m.message_id<?) "
			 "ORDER BY m.message_id DESC LIMIT ?").c_str()));
		query->setInt64(1, thread_id);
		query->setInt64(2, before_message_id);
		query->setInt64(3, before_message_id);
		query->setInt(4, page_size + 1);
		auto result = std::unique_ptr<sql::ResultSet>(query->executeQuery());
		auto page = std::make_shared<PageResult>();
		while (result->next()) page->messages.push_back(*ReadMessage(result.get()));
		if (static_cast<int>(page->messages.size()) > page_size) {
			page->messages.pop_back();
			page->load_more = true;
		}
		page->next_cursor = page->messages.empty() ? before_message_id
			: page->messages.back().message_id;
		return page;
	} catch (const sql::SQLException& error) {
		std::cerr << "LoadChatMsg SQLException: " << error.what() << std::endl;
		return nullptr;
	}
}

std::shared_ptr<ChatMessage> MysqlDao::GetChatMsgById(std::int64_t message_id) {
	auto connection = pool_->getConnection();
	if (!connection) return nullptr;
	Defer defer([this, &connection]() { pool_->returnConnection(std::move(connection)); });
	try {
		auto query = std::unique_ptr<sql::PreparedStatement>(connection->_con->prepareStatement(
			(std::string("SELECT ") + kMessageProjection +
			 "FROM chat_messages m JOIN private_chats pc ON pc.thread_id=m.thread_id "
			 "LEFT JOIN message_resources r ON r.message_id=m.message_id WHERE m.message_id=?").c_str()));
		query->setInt64(1, message_id);
		auto result = std::unique_ptr<sql::ResultSet>(query->executeQuery());
		return result->next() ? ReadMessage(result.get()) : nullptr;
	} catch (const sql::SQLException&) {
		return nullptr;
	}
}

std::shared_ptr<FriendRequest> MysqlDao::GetFriendRequestById(
	std::int64_t friend_request_id) {
	auto connection = pool_->getConnection();
	if (!connection) return nullptr;
	Defer defer([this, &connection]() { pool_->returnConnection(std::move(connection)); });
	try {
		auto query = std::unique_ptr<sql::PreparedStatement>(connection->_con->prepareStatement(
			(std::string("SELECT ") + kFriendRequestColumns +
			 " FROM friend_requests WHERE friend_request_id=?").c_str()));
		query->setInt64(1, friend_request_id);
		auto result = std::unique_ptr<sql::ResultSet>(query->executeQuery());
		return result->next() ? ReadFriendRequest(result.get()) : nullptr;
	} catch (const sql::SQLException&) {
		return nullptr;
	}
}

bool MysqlDao::GetEventsAfterSeq(int recipient_user_id,
	std::uint64_t after_event_seq, int limit, std::vector<UserEvent>& events) {
	events.clear();
	auto connection = pool_->getConnection();
	if (!connection) return false;
	Defer defer([this, &connection]() { pool_->returnConnection(std::move(connection)); });
	try {
		auto query = std::unique_ptr<sql::PreparedStatement>(connection->_con->prepareStatement(
			"SELECT event_seq,event_type,message_id,friend_request_id FROM user_events "
			"WHERE recipient_user_id=? AND event_seq>? ORDER BY event_seq ASC LIMIT ?"));
		query->setInt(1, recipient_user_id);
		query->setUInt64(2, after_event_seq);
		query->setInt(3, limit + 1);
		auto result = std::unique_ptr<sql::ResultSet>(query->executeQuery());
		while (result->next()) {
			UserEvent event;
			event.event_seq = result->getUInt64("event_seq");
			event.event_type = result->getInt("event_type");
			if (!result->isNull("message_id")) {
				auto message_query = std::unique_ptr<sql::PreparedStatement>(connection->_con->prepareStatement(
					(std::string("SELECT ") + kMessageProjection +
					 "FROM chat_messages m JOIN private_chats pc ON pc.thread_id=m.thread_id "
					 "LEFT JOIN message_resources r ON r.message_id=m.message_id "
					 "WHERE m.message_id=? AND m.status=1").c_str()));
				message_query->setInt64(1, result->getInt64("message_id"));
				auto message_result = std::unique_ptr<sql::ResultSet>(message_query->executeQuery());
				if (!message_result->next()) return false;
				event.message = ReadMessage(message_result.get());
				event.message->event_seq = event.event_seq;
			} else {
				auto friend_query = std::unique_ptr<sql::PreparedStatement>(connection->_con->prepareStatement(
					(std::string("SELECT ") + kFriendRequestColumns +
					 " FROM friend_requests WHERE friend_request_id=?").c_str()));
				friend_query->setInt64(1, result->getInt64("friend_request_id"));
				auto friend_result = std::unique_ptr<sql::ResultSet>(friend_query->executeQuery());
				if (!friend_result->next()) return false;
				event.friend_request = ReadFriendRequest(friend_result.get());
				event.friend_request->event_seq = event.event_seq;
			}
			events.push_back(std::move(event));
		}
		return true;
	} catch (const sql::SQLException& error) {
		std::cerr << "GetEventsAfterSeq SQLException: " << error.what() << std::endl;
		events.clear();
		return false;
	}
}

bool MysqlDao::GetUserEvent(int recipient_user_id, int event_type,
	std::int64_t message_id, std::int64_t friend_request_id, UserEvent& event) {
	event = UserEvent();
	const bool is_message = event_type == static_cast<int>(ChatMsgType::TEXT) ||
		event_type == static_cast<int>(ChatMsgType::PIC) ||
		event_type == static_cast<int>(ChatMsgType::FILE);
	if (is_message != (message_id > 0) || is_message == (friend_request_id > 0)) {
		return false;
	}
	auto connection = pool_->getConnection();
	if (!connection) return false;
	Defer defer([this, &connection]() { pool_->returnConnection(std::move(connection)); });
	try {
		std::string sql = "SELECT event_seq FROM user_events WHERE recipient_user_id=? "
			"AND event_type=? AND ";
		sql += is_message ? "message_id=?" : "friend_request_id=?";
		auto query = std::unique_ptr<sql::PreparedStatement>(
			connection->_con->prepareStatement(sql));
		query->setInt(1, recipient_user_id);
		query->setInt(2, event_type);
		query->setInt64(3, is_message ? message_id : friend_request_id);
		auto result = std::unique_ptr<sql::ResultSet>(query->executeQuery());
		if (!result->next()) return false;
		event.event_seq = result->getUInt64("event_seq");
		event.event_type = event_type;
		if (is_message) {
			event.message = GetChatMsgById(message_id);
			if (!event.message || event.message->recipient_user_id != recipient_user_id ||
				event.message->status != MessageStatus::Published) return false;
			event.message->event_seq = event.event_seq;
		} else {
			event.friend_request = GetFriendRequestById(friend_request_id);
			if (!event.friend_request) return false;
			event.friend_request->event_seq = event.event_seq;
		}
		return true;
	} catch (const sql::SQLException&) {
		return false;
	}
}

bool MysqlDao::GetLastEventSeq(int user_id, std::uint64_t& last_event_seq) {
	last_event_seq = 0;
	auto connection = pool_->getConnection();
	if (!connection) return false;
	Defer defer([this, &connection]() { pool_->returnConnection(std::move(connection)); });
	try {
		auto query = std::unique_ptr<sql::PreparedStatement>(connection->_con->prepareStatement(
			"SELECT last_event_seq FROM users WHERE user_id=?"));
		query->setInt(1, user_id);
		auto result = std::unique_ptr<sql::ResultSet>(query->executeQuery());
		if (!result->next()) return false;
		last_event_seq = result->getUInt64("last_event_seq");
		return true;
	} catch (const sql::SQLException&) {
		return false;
	}
}
