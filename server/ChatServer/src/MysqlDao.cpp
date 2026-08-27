#include "MysqlDao.h"
#include "ConfigMgr.h"
#include "PasswordHash.h"
#include "utils.h"

#include <algorithm>

namespace {
constexpr const char* kMessageColumns =
	"message_id, thread_id, recv_seq, sender_id, recv_id, content, created_at, "
	"status, msg_type, resource_status, unique_id, content_size, content_hash, "
	"mime_type, business_status, related_message_id, handled_at, requester_remark";
} // namespace

MysqlDao::MysqlDao()
{
	auto& cfg = ConfigMgr::Inst();
	const auto& host = cfg["Mysql"]["Host"];
	const auto& port = cfg["Mysql"]["Port"];
	const auto& pwd = cfg["Mysql"]["Passwd"];
	const auto& schema = cfg["Mysql"]["Schema"];
	const auto& user = cfg["Mysql"]["User"];
	pool_.reset(new MySqlPool(host + ":" + port, user, pwd, schema, 5));
}

MysqlDao::~MysqlDao() {
	pool_->Close();
}

bool MysqlDao::AllocateRecvSeq(sql::Connection* conn, int uid, std::uint64_t& recv_seq) {
	auto select = std::unique_ptr<sql::PreparedStatement>(conn->prepareStatement(
		"SELECT last_recv_seq FROM user WHERE uid = ? FOR UPDATE"));
	select->setInt(1, uid);
	auto rs = std::unique_ptr<sql::ResultSet>(select->executeQuery());
	if (!rs->next()) {
		return false;
	}
	recv_seq = rs->getUInt64("last_recv_seq") + 1;
	auto update = std::unique_ptr<sql::PreparedStatement>(conn->prepareStatement(
		"UPDATE user SET last_recv_seq = ? WHERE uid = ?"));
	update->setUInt64(1, recv_seq);
	update->setInt(2, uid);
	return update->executeUpdate() == 1;
}

std::shared_ptr<ChatMessage> MysqlDao::ReadMessage(sql::ResultSet* rs) {
	if (!rs) {
		return nullptr;
	}
	auto msg = std::make_shared<ChatMessage>();
	msg->message_id = rs->getInt64("message_id");
	msg->thread_id = rs->isNull("thread_id") ? 0 : rs->getInt64("thread_id");
	msg->recv_seq = rs->isNull("recv_seq") ? 0 : rs->getUInt64("recv_seq");
	msg->sender_id = rs->getInt("sender_id");
	msg->recv_id = rs->getInt("recv_id");
	msg->content = rs->getString("content");
	msg->chat_time = rs->getString("created_at");
	msg->status = rs->getInt("status");
	msg->msg_type = rs->getInt("msg_type");
	msg->resource_status = static_cast<ResourceStatus>(rs->getInt("resource_status"));
	msg->unique_id = rs->isNull("unique_id") ? "" : rs->getString("unique_id");
	msg->content_size = rs->getUInt64("content_size");
	msg->content_hash = rs->isNull("content_hash") ? "" : rs->getString("content_hash");
	msg->mime_type = rs->isNull("mime_type") ? "" : rs->getString("mime_type");
	msg->business_status = static_cast<BusinessStatus>(rs->getInt("business_status"));
	msg->related_message_id = rs->isNull("related_message_id") ? 0 : rs->getInt64("related_message_id");
	msg->handled_at = rs->isNull("handled_at") ? "" : rs->getString("handled_at");
	msg->requester_remark = rs->isNull("requester_remark") ? "" : rs->getString("requester_remark");
	return msg;
}

int MysqlDao::RegUser(const std::string& name, const std::string& email, const std::string& pwd)
{
	auto con = pool_->getConnection();
	try {
		if (con == nullptr) {
			return false;
		}
		// 准备调用存储过程
		std::unique_ptr < sql::PreparedStatement > stmt(con->_con->prepareStatement("CALL reg_user(?,?,?,@result)"));
		// 密码先做 PBKDF2 哈希，DB 只存哈希
		const std::string hashed = llfc::HashPassword(pwd);
		if (hashed.empty()) {
			pool_->returnConnection(std::move(con));
			return -1;
		}
		// 设置输入参数
		stmt->setString(1, name);
		stmt->setString(2, email);
		stmt->setString(3, hashed);

		// 由于PreparedStatement不直接支持注册输出参数，我们需要使用会话变量或其他方法来获取输出参数的值

		  // 执行存储过程
		stmt->execute();
		// 如果存储过程设置了会话变量或有其他方式获取输出参数的值，你可以在这里执行SELECT查询来获取它们
	   // 例如，如果存储过程设置了一个会话变量@result来存储输出结果，可以这样获取：
		std::unique_ptr<sql::Statement> stmtResult(con->_con->createStatement());
		std::unique_ptr<sql::ResultSet> res(stmtResult->executeQuery("SELECT @result AS result"));
		if (res->next()) {
			int result = res->getInt("result");
			std::cout << "Result: " << result << std::endl;
			pool_->returnConnection(std::move(con));
			return result;
		}
		pool_->returnConnection(std::move(con));
		return -1;
	}
	catch (sql::SQLException& e) {
		pool_->returnConnection(std::move(con));
		std::cerr << "SQLException: " << e.what();
		std::cerr << " (MySQL error code: " << e.getErrorCode();
		std::cerr << ", SQLState: " << e.getSQLState() << " )" << std::endl;
		return -1;
	}
}

bool MysqlDao::CheckEmail(const std::string& name, const std::string& email) {
	auto con = pool_->getConnection();
	try {
		if (con == nullptr) {
			return false;
		}

		// 准备查询语句
		std::unique_ptr<sql::PreparedStatement> pstmt(con->_con->prepareStatement("SELECT email FROM user WHERE name = ?"));

		// 绑定参数
		pstmt->setString(1, name);

		// 执行查询
		std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());

		// 遍历结果集
		while (res->next()) {
			std::cout << "Check Email: " << res->getString("email") << std::endl;
			if (email != res->getString("email")) {
				pool_->returnConnection(std::move(con));
				return false;
			}
			pool_->returnConnection(std::move(con));
			return true;
		}
		return true;
	}
	catch (sql::SQLException& e) {
		pool_->returnConnection(std::move(con));
		std::cerr << "SQLException: " << e.what();
		std::cerr << " (MySQL error code: " << e.getErrorCode();
		std::cerr << ", SQLState: " << e.getSQLState() << " )" << std::endl;
		return false;
	}
}

bool MysqlDao::UpdatePwd(const std::string& name, const std::string& newpwd) {
	auto con = pool_->getConnection();
	try {
		if (con == nullptr) {
			return false;
		}

		// 密码先做 PBKDF2 哈希，DB 只存哈希
		const std::string hashed = llfc::HashPassword(newpwd);
		if (hashed.empty()) {
			pool_->returnConnection(std::move(con));
			return false;
		}
		// 准备查询语句
		std::unique_ptr<sql::PreparedStatement> pstmt(con->_con->prepareStatement("UPDATE user SET pwd = ? WHERE name = ?"));

		// 绑定参数
		pstmt->setString(2, name);
		pstmt->setString(1, hashed);

		// 执行更新
		int updateCount = pstmt->executeUpdate();

		std::cout << "Updated rows: " << updateCount << std::endl;
		pool_->returnConnection(std::move(con));
		return true;
	}
	catch (sql::SQLException& e) {
		pool_->returnConnection(std::move(con));
		std::cerr << "SQLException: " << e.what();
		std::cerr << " (MySQL error code: " << e.getErrorCode();
		std::cerr << ", SQLState: " << e.getSQLState() << " )" << std::endl;
		return false;
	}
}

bool MysqlDao::CheckPwd(const std::string& name, const std::string& pwd, UserInfo& userInfo) {
	auto con = pool_->getConnection();
	if (con == nullptr) {
		return false;
	}

	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});

	try {
		// 准备SQL语句
		std::unique_ptr<sql::PreparedStatement> pstmt(con->_con->prepareStatement("SELECT * FROM user WHERE name = ?"));
		pstmt->setString(1, name); // 将username替换为你要查询的用户名

		// 执行查询
		std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
		std::string origin_pwd = "";
		// 遍历结果集
		while (res->next()) {
			origin_pwd = res->getString("pwd");
			break;
		}

		// PBKDF2 恒定时间校验；pwd 必须为 pbkdf2-sha256 格式，明文/畸形值一律拒绝
		if (!llfc::VerifyPassword(pwd, origin_pwd)) {
			return false;
		}
		userInfo.name = name;
		userInfo.email = res->getString("email");
		userInfo.uid = res->getInt("uid");
		return true;
	}
	catch (sql::SQLException& e) {
		std::cerr << "SQLException: " << e.what();
		std::cerr << " (MySQL error code: " << e.getErrorCode();
		std::cerr << ", SQLState: " << e.getSQLState() << " )" << std::endl;
		return false;
	}
}

FriendOperationResult MysqlDao::AddFriendApply(int from, int to,
	const std::string& desc, const std::string& requester_remark,
	const std::string& unique_id, std::shared_ptr<ChatMessage>& application) {
	application.reset();
	if (from <= 0 || to <= 0 || from == to || unique_id.empty()) {
		return FriendOperationResult::Failed;
	}
	auto con = pool_->getConnection();
	if (!con) {
		return FriendOperationResult::Failed;
	}
	Defer defer([this, &con]() { pool_->returnConnection(std::move(con)); });
	auto& conn = con->_con;
	try {
		conn->setAutoCommit(false);
		auto serialize = std::unique_ptr<sql::PreparedStatement>(conn->prepareStatement(
			"SELECT last_recv_seq FROM user WHERE uid = ? FOR UPDATE"));
		serialize->setInt(1, to);
		auto serialize_rs = std::unique_ptr<sql::ResultSet>(serialize->executeQuery());
		if (!serialize_rs->next()) {
			conn->rollback();
			return FriendOperationResult::NotFound;
		}

		// The request id owns idempotency even after an application has been handled.
		// A deliberate re-application must use a new request id.
		auto retry = std::unique_ptr<sql::PreparedStatement>(conn->prepareStatement(
			(std::string("SELECT ") + kMessageColumns +
			 " FROM chat_message WHERE sender_id = ? AND unique_id = ? LIMIT 1 FOR UPDATE").c_str()));
		retry->setInt(1, from);
		retry->setString(2, unique_id);
		auto retry_rs = std::unique_ptr<sql::ResultSet>(retry->executeQuery());
		if (retry_rs->next()) {
			application = ReadMessage(retry_rs.get());
			if (!application || application->msg_type != static_cast<int>(ChatMsgType::FRIEND_APPLY) ||
				application->recv_id != to || application->content != desc ||
				application->requester_remark != requester_remark) {
				conn->rollback();
				return FriendOperationResult::Conflict;
			}
			conn->commit();
			return FriendOperationResult::Duplicate;
		}

		auto friends = std::unique_ptr<sql::PreparedStatement>(conn->prepareStatement(
			"SELECT 1 FROM friend WHERE self_id = ? AND friend_id = ? LIMIT 1 FOR UPDATE"));
		friends->setInt(1, from);
		friends->setInt(2, to);
		auto friend_rs = std::unique_ptr<sql::ResultSet>(friends->executeQuery());
		if (friend_rs->next()) {
			conn->rollback();
			return FriendOperationResult::AlreadyFriends;
		}

		auto existing = std::unique_ptr<sql::PreparedStatement>(conn->prepareStatement(
			(std::string("SELECT ") + kMessageColumns +
			 " FROM chat_message WHERE sender_id = ? AND recv_id = ? "
			 "AND msg_type = 10 AND business_status = 1 LIMIT 1 FOR UPDATE").c_str()));
		existing->setInt(1, from);
		existing->setInt(2, to);
		auto existing_rs = std::unique_ptr<sql::ResultSet>(existing->executeQuery());
		if (existing_rs->next()) {
			application = ReadMessage(existing_rs.get());
			conn->commit();
			return FriendOperationResult::Duplicate;
		}

		std::uint64_t recv_seq = 0;
		if (!AllocateRecvSeq(conn.get(), to, recv_seq)) {
			conn->rollback();
			return FriendOperationResult::Failed;
		}

		auto insert = std::unique_ptr<sql::PreparedStatement>(conn->prepareStatement(
			"INSERT INTO chat_message(thread_id, sender_id, recv_id, recv_seq, content, "
			"created_at, updated_at, status, msg_type, resource_status, business_status, "
			"unique_id, content_size, requester_remark) "
			"VALUES(NULL, ?, ?, ?, ?, NOW(3), NOW(3), 0, 10, 0, 1, ?, 0, ?)"));
		insert->setInt(1, from);
		insert->setInt(2, to);
		insert->setUInt64(3, recv_seq);
		insert->setString(4, desc);
		insert->setString(5, unique_id);
		insert->setString(6, requester_remark);
		insert->executeUpdate();
		auto key = std::unique_ptr<sql::Statement>(conn->createStatement());
		auto key_rs = std::unique_ptr<sql::ResultSet>(key->executeQuery("SELECT LAST_INSERT_ID()"));
		if (!key_rs->next()) {
			conn->rollback();
			return FriendOperationResult::Failed;
		}
		application = std::make_shared<ChatMessage>();
		application->message_id = key_rs->getInt64(1);
		application->thread_id = 0;
		application->recv_seq = recv_seq;
		application->sender_id = from;
		application->recv_id = to;
		application->content = desc;
		application->chat_time = getCurrentTimestamp();
		application->status = 0;
		application->msg_type = static_cast<int>(ChatMsgType::FRIEND_APPLY);
		application->business_status = BusinessStatus::Pending;
		application->unique_id = unique_id;
		application->requester_remark = requester_remark;
		conn->commit();
		return FriendOperationResult::Stored;
	}
	catch (const sql::SQLException& e) {
		conn->rollback();
		std::cerr << "AddFriendApply SQLException: " << e.what() << std::endl;
		return FriendOperationResult::Failed;
	}
}

FriendOperationResult MysqlDao::HandleFriendApply(int handler_uid,
	std::int64_t apply_message_id, bool accept, const std::string& handler_remark,
	const std::string& reason, FriendHandleOutput& output) {
	output = FriendHandleOutput{};
	auto con = pool_->getConnection();
	if (!con) {
		return FriendOperationResult::Failed;
	}
	Defer defer([this, &con]() { pool_->returnConnection(std::move(con)); });
	auto& conn = con->_con;
	try {
		conn->setAutoCommit(false);
		auto select = std::unique_ptr<sql::PreparedStatement>(conn->prepareStatement(
			(std::string("SELECT ") + kMessageColumns +
			 " FROM chat_message WHERE message_id = ? FOR UPDATE").c_str()));
		select->setInt64(1, apply_message_id);
		auto rs = std::unique_ptr<sql::ResultSet>(select->executeQuery());
		if (!rs->next()) {
			conn->rollback();
			return FriendOperationResult::NotFound;
		}
		output.application = ReadMessage(rs.get());
		if (!output.application ||
			output.application->msg_type != static_cast<int>(ChatMsgType::FRIEND_APPLY) ||
			output.application->recv_id != handler_uid) {
			conn->rollback();
			return FriendOperationResult::Forbidden;
		}
		output.peer_uid = output.application->sender_id;

		if (output.application->business_status != BusinessStatus::Pending) {
			auto result_stmt = std::unique_ptr<sql::PreparedStatement>(conn->prepareStatement(
				(std::string("SELECT ") + kMessageColumns +
				 " FROM chat_message WHERE related_message_id = ?").c_str()));
			result_stmt->setInt64(1, apply_message_id);
			auto result_rs = std::unique_ptr<sql::ResultSet>(result_stmt->executeQuery());
			if (result_rs->next()) {
				output.result_message = ReadMessage(result_rs.get());
				output.thread_id = output.result_message->thread_id;
			}
			const bool same = output.result_message && (
				(accept && output.application->business_status == BusinessStatus::Accepted) ||
				(!accept && output.application->business_status == BusinessStatus::Rejected));
			if (same && accept) {
				auto remark_stmt = std::unique_ptr<sql::PreparedStatement>(conn->prepareStatement(
					"SELECT back FROM friend WHERE self_id = ? AND friend_id = ? LIMIT 1"));
				remark_stmt->setInt(1, handler_uid);
				remark_stmt->setInt(2, output.peer_uid);
				auto remark_rs = std::unique_ptr<sql::ResultSet>(remark_stmt->executeQuery());
				if (remark_rs->next()) {
					output.handler_contact_remark = remark_rs->getString("back");
				}
			}
			conn->commit();
			return same ? FriendOperationResult::Duplicate : FriendOperationResult::AlreadyHandled;
		}

		const int applicant_uid = output.application->sender_id;
		std::int64_t thread_id = 0;
		if (accept) {
			const int private_uid1 = std::min(handler_uid, applicant_uid);
			const int private_uid2 = std::max(handler_uid, applicant_uid);
			output.handler_contact_remark = handler_remark;
			auto friendship = std::unique_ptr<sql::PreparedStatement>(conn->prepareStatement(
				"SELECT 1 FROM friend WHERE self_id = ? AND friend_id = ? LIMIT 1 FOR UPDATE"));
			friendship->setInt(1, handler_uid);
			friendship->setInt(2, applicant_uid);
			auto friendship_rs = std::unique_ptr<sql::ResultSet>(friendship->executeQuery());
			if (friendship_rs->next()) {
				conn->rollback();
				return FriendOperationResult::AlreadyFriends;
			}

			auto add_friend = std::unique_ptr<sql::PreparedStatement>(conn->prepareStatement(
				"INSERT INTO friend(self_id, friend_id, back) VALUES(?, ?, ?), (?, ?, ?)"));
			add_friend->setInt(1, handler_uid);
			add_friend->setInt(2, applicant_uid);
			add_friend->setString(3, handler_remark);
			add_friend->setInt(4, applicant_uid);
			add_friend->setInt(5, handler_uid);
			add_friend->setString(6, output.application->requester_remark);
			add_friend->executeUpdate();

			auto find_thread = std::unique_ptr<sql::PreparedStatement>(conn->prepareStatement(
				"SELECT thread_id FROM private_chat WHERE "
				"(user1_id = ? AND user2_id = ?) OR (user1_id = ? AND user2_id = ?) "
				"LIMIT 1 FOR UPDATE"));
			find_thread->setInt(1, handler_uid);
			find_thread->setInt(2, applicant_uid);
			find_thread->setInt(3, applicant_uid);
			find_thread->setInt(4, handler_uid);
			auto thread_rs = std::unique_ptr<sql::ResultSet>(find_thread->executeQuery());
			if (thread_rs->next()) {
				thread_id = thread_rs->getInt64("thread_id");
			}
			else {
				auto create_thread = std::unique_ptr<sql::PreparedStatement>(conn->prepareStatement(
					"INSERT INTO chat_thread(type, created_at) VALUES('private', NOW(3))"));
				create_thread->executeUpdate();
				auto key = std::unique_ptr<sql::Statement>(conn->createStatement());
				auto key_rs = std::unique_ptr<sql::ResultSet>(key->executeQuery("SELECT LAST_INSERT_ID()"));
				if (!key_rs->next()) {
					conn->rollback();
					return FriendOperationResult::Failed;
				}
				thread_id = key_rs->getInt64(1);
				auto private_chat = std::unique_ptr<sql::PreparedStatement>(conn->prepareStatement(
					"INSERT INTO private_chat(thread_id, user1_id, user2_id) VALUES(?, ?, ?)"));
				private_chat->setInt64(1, thread_id);
				private_chat->setInt(2, private_uid1);
				private_chat->setInt(3, private_uid2);
				private_chat->executeUpdate();
			}

			auto close_reverse = std::unique_ptr<sql::PreparedStatement>(conn->prepareStatement(
				"UPDATE chat_message SET business_status = 2, handled_at = NOW(3) "
				"WHERE msg_type = 10 AND business_status = 1 AND "
				"((sender_id = ? AND recv_id = ?) OR (sender_id = ? AND recv_id = ?))"));
			close_reverse->setInt(1, applicant_uid);
			close_reverse->setInt(2, handler_uid);
			close_reverse->setInt(3, handler_uid);
			close_reverse->setInt(4, applicant_uid);
			close_reverse->executeUpdate();
		}
		else {
			auto reject = std::unique_ptr<sql::PreparedStatement>(conn->prepareStatement(
				"UPDATE chat_message SET business_status = 3, handled_at = NOW(3) "
				"WHERE message_id = ? AND business_status = 1"));
			reject->setInt64(1, apply_message_id);
			if (reject->executeUpdate() != 1) {
				conn->rollback();
				return FriendOperationResult::AlreadyHandled;
			}
		}

		std::uint64_t recv_seq = 0;
		if (!AllocateRecvSeq(conn.get(), applicant_uid, recv_seq)) {
			conn->rollback();
			return FriendOperationResult::Failed;
		}
		const int result_type = accept
			? static_cast<int>(ChatMsgType::FRIEND_ACCEPT)
			: static_cast<int>(ChatMsgType::FRIEND_REJECT);
		auto insert_result = std::unique_ptr<sql::PreparedStatement>(conn->prepareStatement(
			"INSERT INTO chat_message(thread_id, sender_id, recv_id, recv_seq, content, "
			"created_at, updated_at, status, msg_type, resource_status, business_status, "
			"related_message_id, handled_at, requester_remark, content_size) "
			"VALUES(?, ?, ?, ?, ?, NOW(3), NOW(3), 2, ?, 0, ?, ?, NOW(3), ?, 0)"));
		if (accept) insert_result->setInt64(1, thread_id);
		else insert_result->setNull(1, sql::DataType::BIGINT);
		insert_result->setInt(2, handler_uid);
		insert_result->setInt(3, applicant_uid);
		insert_result->setUInt64(4, recv_seq);
		insert_result->setString(5, accept ? "We are friends now!" : reason);
		insert_result->setInt(6, result_type);
		insert_result->setInt(7, accept
			? static_cast<int>(BusinessStatus::Accepted)
			: static_cast<int>(BusinessStatus::Rejected));
		insert_result->setInt64(8, apply_message_id);
		if (accept) insert_result->setString(9, output.application->requester_remark);
		else insert_result->setNull(9, sql::DataType::VARCHAR);
		insert_result->executeUpdate();
		auto key = std::unique_ptr<sql::Statement>(conn->createStatement());
		auto key_rs = std::unique_ptr<sql::ResultSet>(key->executeQuery("SELECT LAST_INSERT_ID()"));
		if (!key_rs->next()) {
			conn->rollback();
			return FriendOperationResult::Failed;
		}
		output.result_message = std::make_shared<ChatMessage>();
		output.result_message->message_id = key_rs->getInt64(1);
		output.result_message->thread_id = thread_id;
		output.result_message->recv_seq = recv_seq;
		output.result_message->sender_id = handler_uid;
		output.result_message->recv_id = applicant_uid;
		output.result_message->content = accept ? "We are friends now!" : reason;
		output.result_message->chat_time = getCurrentTimestamp();
		output.result_message->status = 2;
		output.result_message->msg_type = result_type;
		output.result_message->business_status = accept
			? BusinessStatus::Accepted : BusinessStatus::Rejected;
		output.result_message->related_message_id = apply_message_id;
		output.result_message->requester_remark = accept
			? output.application->requester_remark : std::string();
		output.result_message->handled_at = getCurrentTimestamp();
		output.application->business_status = accept
			? BusinessStatus::Accepted : BusinessStatus::Rejected;
		output.application->handled_at = output.result_message->handled_at;
		output.thread_id = thread_id;
		conn->commit();
		return FriendOperationResult::Stored;
	}
	catch (const sql::SQLException& e) {
		conn->rollback();
		std::cerr << "HandleFriendApply SQLException: " << e.what() << std::endl;
		return FriendOperationResult::Failed;
	}
}

std::shared_ptr<UserInfo> MysqlDao::GetUser(int uid)
{
	auto con = pool_->getConnection();
	if (con == nullptr) {
		return nullptr;
	}

	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});

	try {
		// 准备SQL语句
		std::unique_ptr<sql::PreparedStatement> pstmt(con->_con->prepareStatement("SELECT * FROM user WHERE uid = ?"));
		pstmt->setInt(1, uid); // 将uid替换为你要查询的uid

		// 执行查询
		std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
		std::shared_ptr<UserInfo> user_ptr = nullptr;
		// 遍历结果集
		while (res->next()) {
			user_ptr.reset(new UserInfo);
			user_ptr->email = res->getString("email");
			user_ptr->name = res->getString("name");
			user_ptr->nick = res->getString("nick");
			user_ptr->desc = res->getString("desc");
			user_ptr->sex = res->getInt("sex");
			user_ptr->icon = res->getString("icon");
			user_ptr->uid = uid;
			break;
		}
		return user_ptr;
	}
	catch (sql::SQLException& e) {
		std::cerr << "SQLException: " << e.what();
		std::cerr << " (MySQL error code: " << e.getErrorCode();
		std::cerr << ", SQLState: " << e.getSQLState() << " )" << std::endl;
		return nullptr;
	}
}

std::shared_ptr<UserInfo> MysqlDao::GetUser(std::string name)
{
	auto con = pool_->getConnection();
	if (con == nullptr) {
		return nullptr;
	}

	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});

	try {
		// 准备SQL语句
		std::unique_ptr<sql::PreparedStatement> pstmt(con->_con->prepareStatement("SELECT * FROM user WHERE name = ?"));
		pstmt->setString(1, name); // 将uid替换为你要查询的uid

		// 执行查询
		std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
		std::shared_ptr<UserInfo> user_ptr = nullptr;
		// 遍历结果集
		while (res->next()) {
			user_ptr.reset(new UserInfo);
			user_ptr->email = res->getString("email");
			user_ptr->name = res->getString("name");
			user_ptr->nick = res->getString("nick");
			user_ptr->desc = res->getString("desc");
			user_ptr->sex = res->getInt("sex");
			user_ptr->uid = res->getInt("uid");
			user_ptr->icon = res->getString("icon");
			break;
		}
		return user_ptr;
	}
	catch (sql::SQLException& e) {
		std::cerr << "SQLException: " << e.what();
		std::cerr << " (MySQL error code: " << e.getErrorCode();
		std::cerr << ", SQLState: " << e.getSQLState() << " )" << std::endl;
		return nullptr;
	}
}


bool MysqlDao::GetApplyList(int touid, std::vector<std::shared_ptr<ApplyInfo>>& applyList,
	std::int64_t after_message_id, int limit) {
	auto con = pool_->getConnection();
	if (con == nullptr) {
		return false;
	}

	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});


	try {
		std::unique_ptr<sql::PreparedStatement> pstmt(con->_con->prepareStatement(
			"SELECT m.message_id, m.sender_id, m.recv_id, m.content, m.requester_remark, "
			"m.business_status, m.created_at, u.name, u.nick, u.sex, u.icon, u.`desc` "
			"FROM chat_message m JOIN user u ON u.uid = "
			"(CASE WHEN m.sender_id = ? THEN m.recv_id ELSE m.sender_id END) "
			"WHERE (m.sender_id = ? OR m.recv_id = ?) AND m.msg_type = 10 "
			"AND m.business_status = 1 AND m.message_id > ? "
			"ORDER BY m.message_id ASC LIMIT ?"));

		pstmt->setInt(1, touid);
		pstmt->setInt(2, touid);
		pstmt->setInt(3, touid);
		pstmt->setInt64(4, after_message_id);
		pstmt->setInt(5, limit);
		// 执行查询
		std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
		// 遍历结果集
		while (res->next()) {
			auto name = res->getString("name");
			auto from_uid = res->getInt("sender_id");
			auto to_uid = res->getInt("recv_id");
			auto status = res->getInt("business_status");
			auto nick = res->getString("nick");
			auto sex = res->getInt("sex");
			auto apply_ptr = std::make_shared<ApplyInfo>(res->getInt64("message_id"),
				from_uid, to_uid, name, res->getString("content"),
				res->isNull("requester_remark") ? "" : res->getString("requester_remark"),
				res->getString("icon"), nick, res->getString("desc"),
				res->getString("created_at"), sex, status);
			applyList.push_back(apply_ptr);
		}
		return true;
	}
	catch (sql::SQLException& e) {
		std::cerr << "SQLException: " << e.what();
		std::cerr << " (MySQL error code: " << e.getErrorCode();
		std::cerr << ", SQLState: " << e.getSQLState() << " )" << std::endl;
		return false;
	}
}

bool MysqlDao::GetFriendList(int self_id, std::vector<std::shared_ptr<UserInfo> >& user_info_list) {

	auto con = pool_->getConnection();
	if (con == nullptr) {
		return false;
	}

	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});


	try {
		std::unique_ptr<sql::PreparedStatement> pstmt(con->_con->prepareStatement(
			"SELECT u.uid, u.name, u.email, u.nick, u.`desc`, u.sex, u.icon, f.back, "
			"p.thread_id FROM friend f JOIN user u ON u.uid = f.friend_id "
			"LEFT JOIN private_chat p ON ((p.user1_id = f.self_id AND p.user2_id = f.friend_id) "
			"OR (p.user1_id = f.friend_id AND p.user2_id = f.self_id)) WHERE f.self_id = ?"));

		pstmt->setInt(1, self_id); // 将uid替换为你要查询的uid

		// 执行查询
		std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
		// 遍历结果集
		while (res->next()) {
			auto user_info = std::make_shared<UserInfo>();
			user_info->uid = res->getInt("uid");
			user_info->name = res->getString("name");
			user_info->email = res->getString("email");
			user_info->nick = res->getString("nick");
			user_info->desc = res->getString("desc");
			user_info->sex = res->getInt("sex");
			user_info->icon = res->getString("icon");
			const std::string back_text = res->getString("back").asStdString();
			user_info->back = back_text.empty() ? user_info->name : back_text;
			user_info->thread_id = res->isNull("thread_id") ? 0 : res->getInt64("thread_id");
			user_info_list.push_back(user_info);
		}
		return true;
	}
	catch (sql::SQLException& e) {
		std::cerr << "SQLException: " << e.what();
		std::cerr << " (MySQL error code: " << e.getErrorCode();
		std::cerr << ", SQLState: " << e.getSQLState() << " )" << std::endl;
		return false;
	}

	return true;
}

// 新增两个输出参数：loadMore, nextLastId
bool MysqlDao::GetUserThreads(
	int64_t userId,
	int64_t lastId,
	int      pageSize,
	std::vector<std::shared_ptr<ChatThreadInfo>>& threads,
	bool& loadMore,
	int64_t& nextLastId)
{
	// 初始状态
	loadMore = false;
	nextLastId = lastId;
	threads.clear();

	auto con = pool_->getConnection();
	if (!con) {
		return false;
	}
	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});
	auto& conn = con->_con;

	try {
		// 准备分页查询：CTE + UNION ALL + ORDER + LIMIT N+1
		std::string sql =
			"WITH all_threads AS ( "
			"  SELECT thread_id, 'private' AS type, user1_id, user2_id "
			"    FROM private_chat "
			"   WHERE (user1_id = ? OR user2_id = ?) "
			"     AND thread_id > ? "
			"  UNION ALL "
			"  SELECT thread_id, 'group'   AS type, 0 AS user1_id, 0 AS user2_id "
			"    FROM group_chat_member "
			"   WHERE user_id   = ? "
			"     AND thread_id > ? "
			") "
			"SELECT thread_id, type, user1_id, user2_id, "
			"       COALESCE((SELECT MAX(m.message_id) FROM chat_message m "
			"                 WHERE m.thread_id = all_threads.thread_id), 0) AS last_msg_id "
			"  FROM all_threads "
			" ORDER BY thread_id "
			" LIMIT ?;";

		std::unique_ptr<sql::PreparedStatement> pstmt(
			conn->prepareStatement(sql));

		// 绑定参数：? 对应 (userId, userId, lastId, userId, lastId, pageSize+1)
		int idx = 1;
		pstmt->setInt64(idx++, userId);              // private.user1_id
		pstmt->setInt64(idx++, userId);              // private.user2_id
		pstmt->setInt64(idx++, lastId);              // private.thread_id > lastId
		pstmt->setInt64(idx++, userId);              // group.user_id
		pstmt->setInt64(idx++, lastId);              // group.thread_id > lastId
		pstmt->setInt(idx++, pageSize + 1);          // LIMIT pageSize+1

		// 执行
		std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());

		// 先把所有行读到临时容器
		std::vector<std::shared_ptr<ChatThreadInfo>> tmp;
		while (res->next()) {
			auto cti = std::make_shared<ChatThreadInfo>();
			cti->_thread_id = res->getInt64("thread_id");
			cti->_type = res->getString("type");
			cti->_user1_id = res->getInt64("user1_id");
			cti->_user2_id = res->getInt64("user2_id");
			cti->_last_msg_id = res->getInt64("last_msg_id");
			tmp.push_back(cti);
		}

		// 判断是否多取到一条
		if ((int)tmp.size() > pageSize) {
			loadMore = true;
			tmp.pop_back();  // 丢掉第 pageSize+1 条
		}

		// 如果还有数据，更新 nextLastId 为最后一条的 thread_id
		if (!tmp.empty()) {
			nextLastId = tmp.back()->_thread_id;
		}

		// 移入输出向量
		threads = std::move(tmp);
	}
	catch (sql::SQLException& e) {
		std::cerr << "SQLException: " << e.what()
			<< " (MySQL error code: " << e.getErrorCode()
			<< ", SQLState: " << e.getSQLState() << ")\n";
		return false;
	}

	return true;
}

bool MysqlDao::CreatePrivateChat(int user1_id, int user2_id, std::int64_t& thread_id)
{
	auto con = pool_->getConnection();
	if (!con) {
		return false;
	}

	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});

	auto& conn = con->_con;
	int uid1 = std::min(user1_id, user2_id);
	int uid2 = std::max(user1_id, user2_id);
	try {
		// 开启事务
		conn->setAutoCommit(false);
		// 1. 先尝试查询已存在的记录(无锁)
		std::string check_sql =
			"SELECT thread_id FROM private_chat "
			"WHERE user1_id = ? AND user2_id = ?;";

		std::unique_ptr<sql::PreparedStatement> pstmt(conn->prepareStatement(check_sql));
		pstmt->setInt64(1, uid1);
		pstmt->setInt64(2, uid2);
		std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());

		if (res->next()) {
			// 如果已存在，返回该 thread_id
			thread_id = res->getInt64("thread_id");
			conn->commit();  // 提交事务
			return true;
		}

		// 2. 如果未找到，创建新的 chat_thread 和 private_chat 记录
		// 在 chat_thread 表插入新记录
		std::string insert_chat_thread_sql =
			"INSERT INTO chat_thread (type, created_at) VALUES ('private', NOW());";

		std::unique_ptr<sql::PreparedStatement> pstmt_insert_thread(conn->prepareStatement(insert_chat_thread_sql));
		pstmt_insert_thread->executeUpdate();

		// 获取新插入的 thread_id
		std::string get_last_insert_id_sql = "SELECT LAST_INSERT_ID();";
		std::unique_ptr<sql::PreparedStatement> pstmt_last_insert_id(conn->prepareStatement(get_last_insert_id_sql));
		std::unique_ptr<sql::ResultSet> res_last_id(pstmt_last_insert_id->executeQuery());
		res_last_id->next();
		thread_id = res_last_id->getInt64(1);

		// 3. 在 private_chat 表插入新记录
		std::string insert_private_chat_sql =
			"INSERT INTO private_chat (thread_id, user1_id, user2_id, created_at) "
			"VALUES (?, ?, ?, NOW());";


		std::unique_ptr<sql::PreparedStatement> pstmt_insert_private(conn->prepareStatement(insert_private_chat_sql));
		pstmt_insert_private->setInt64(1, thread_id);
		pstmt_insert_private->setInt64(2, uid1);
		pstmt_insert_private->setInt64(3, uid2);
		pstmt_insert_private->executeUpdate();

		// 提交事务
		conn->commit();
		return true;
	}
	catch (sql::SQLException& e) {
		conn->rollback();

		// 检查是否是唯一键冲突 (MySQL error code 1062)
		if (e.getErrorCode() == 1062) {
			// 重新查询已存在的记录
			try {
				conn->setAutoCommit(true);
				std::string retry_sql =
					"SELECT thread_id FROM private_chat "
					"WHERE user1_id = ? AND user2_id = ?;";
				std::unique_ptr<sql::PreparedStatement> pstmt_retry(
					conn->prepareStatement(retry_sql));
				pstmt_retry->setInt64(1, uid1);
				pstmt_retry->setInt64(2, uid2);
				std::unique_ptr<sql::ResultSet> res_retry(pstmt_retry->executeQuery());

				if (res_retry->next()) {
					thread_id = res_retry->getInt64("thread_id");
					return true;
				}
			}
			catch (...) {
				return false;
			}
		}

		std::cerr << "SQLException: " << e.what()
			<< " (Code: " << e.getErrorCode() << ")" << std::endl;
		return false;
	}
	return false;
}

bool MysqlDao::GetPrivateChatMembers(std::int64_t thread_id, int& user1, int& user2) {
	user1 = 0;
	user2 = 0;
	auto con = pool_->getConnection();
	if (!con) {
		return false;
	}
	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});
	auto& conn = con->_con;

	try {
		auto pstmt = std::unique_ptr<sql::PreparedStatement>(
			conn->prepareStatement(
				"SELECT user1_id, user2_id FROM private_chat WHERE thread_id = ?"
			)
		);
		pstmt->setInt64(1, thread_id);
		auto rs = std::unique_ptr<sql::ResultSet>(pstmt->executeQuery());
		if (!rs->next()) {
			return false;
		}
		user1 = static_cast<int>(rs->getUInt64("user1_id"));
		user2 = static_cast<int>(rs->getUInt64("user2_id"));
		return true;
	}
	catch (sql::SQLException& e) {
		std::cerr << "GetPrivateChatMembers SQLException: " << e.what() << std::endl;
		return false;
	}
}

std::shared_ptr<PageResult> MysqlDao::LoadChatMsg(std::int64_t thread_id, std::int64_t last_message_id, int page_size)
{
	auto con = pool_->getConnection();
	if (!con) {
		return nullptr;
	}
	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});
	auto& conn = con->_con;


	try {
		auto page_res = std::make_shared<PageResult>();
		page_res->load_more = false;
		// SQL：多取一条，用于判断是否还有更多
		const std::string sql = R"(
        SELECT message_id, thread_id, recv_seq, sender_id, recv_id, content,
               created_at, updated_at, status, msg_type, resource_status,
               unique_id, content_size, content_hash, mime_type, business_status,
               related_message_id, handled_at, requester_remark
        FROM chat_message
        WHERE thread_id = ?
          AND message_id > ?
        ORDER BY message_id ASC
        LIMIT ?
		)";

		uint32_t fetch_limit = page_size + 1;
		auto pstmt = std::unique_ptr<sql::PreparedStatement>(
			conn->prepareStatement(sql)
			);
		pstmt->setInt64(1, thread_id);
		pstmt->setInt64(2, last_message_id);
		pstmt->setInt(3, fetch_limit);

		auto rs = std::unique_ptr<sql::ResultSet>(pstmt->executeQuery());

		// 读取 fetch_limit 条记录
		while (rs->next()) {
			ChatMessage msg;
			msg.message_id = rs->getUInt64("message_id");
			msg.thread_id = rs->getUInt64("thread_id");
			msg.recv_seq = rs->isNull("recv_seq") ? 0 : rs->getUInt64("recv_seq");
			msg.sender_id = rs->getUInt64("sender_id");
			msg.recv_id = rs->getUInt64("recv_id");
			msg.content = rs->getString("content");
			msg.chat_time = rs->getString("created_at");
			msg.status = rs->getInt("status");
			msg.msg_type = rs->getInt("msg_type");
			msg.resource_status = static_cast<ResourceStatus>(rs->getInt("resource_status"));
			msg.unique_id = rs->getString("unique_id");
			msg.content_size = rs->getUInt64("content_size");
			msg.content_hash = rs->isNull("content_hash") ? "" : rs->getString("content_hash");
			msg.mime_type = rs->isNull("mime_type") ? "" : rs->getString("mime_type");
			msg.business_status = static_cast<BusinessStatus>(rs->getInt("business_status"));
			msg.related_message_id = rs->isNull("related_message_id") ? 0 : rs->getInt64("related_message_id");
			msg.handled_at = rs->isNull("handled_at") ? "" : rs->getString("handled_at");
			msg.requester_remark = rs->isNull("requester_remark") ? "" : rs->getString("requester_remark");
			page_res->messages.push_back(std::move(msg));
		}
		if (page_res->messages.size() > page_size) {
			page_res->messages.pop_back();
			page_res->load_more = true;
		}

		page_res->next_cursor = page_res->messages.empty()
			? last_message_id : page_res->messages.back().message_id;

		return page_res;
	}
	catch (sql::SQLException& e) {
		std::cerr << "SQLException: " << e.what() << std::endl;
		conn->rollback();
		return nullptr;
	}
	return nullptr;

}


SaveMessageResult MysqlDao::UpsertChatMessage(sql::Connection* conn,
	const std::shared_ptr<ChatMessage>& msg,
	std::string& out_conflict_uid) {
	out_conflict_uid.clear();
	const std::uint64_t requested_recv_seq = msg->recv_seq;

	// 幂等 UPSERT：命中 (sender_id, unique_id) 唯一键时，通过 LAST_INSERT_ID(expr)
	// 把 canonical message_id 暴露出来，且不改动原行任何业务字段（不得覆盖原消息）。
	auto pstmt = std::unique_ptr<sql::PreparedStatement>(
		conn->prepareStatement(
			"INSERT INTO chat_message "
			"(thread_id, sender_id, recv_id, recv_seq, content, created_at, updated_at, "
			" status, msg_type, resource_status, business_status, related_message_id, "
			" handled_at, requester_remark, unique_id, content_size, content_hash, mime_type) "
			"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
			"ON DUPLICATE KEY UPDATE message_id = LAST_INSERT_ID(message_id)"
		)
	);
	if (msg->thread_id > 0) pstmt->setUInt64(1, msg->thread_id);
	else pstmt->setNull(1, sql::DataType::BIGINT);
	pstmt->setUInt64(2, msg->sender_id);
	pstmt->setUInt64(3, msg->recv_id);
	if (msg->recv_seq > 0) pstmt->setUInt64(4, msg->recv_seq);
	else pstmt->setNull(4, sql::DataType::BIGINT);
	pstmt->setString(5, msg->content);
	pstmt->setString(6, msg->chat_time);
	pstmt->setString(7, msg->chat_time);
	pstmt->setInt(8, msg->status);
	pstmt->setInt(9, msg->msg_type);
	pstmt->setInt(10, static_cast<int>(msg->resource_status));
	pstmt->setInt(11, static_cast<int>(msg->business_status));
	if (msg->related_message_id > 0) pstmt->setInt64(12, msg->related_message_id);
	else pstmt->setNull(12, sql::DataType::BIGINT);
	if (!msg->handled_at.empty()) pstmt->setString(13, msg->handled_at);
	else pstmt->setNull(13, sql::DataType::TIMESTAMP);
	if (!msg->requester_remark.empty()) pstmt->setString(14, msg->requester_remark);
	else pstmt->setNull(14, sql::DataType::VARCHAR);
	// 客户端消息 unique_id 非空；空串时写 NULL，避免同一 sender 的空串互相撞唯一键
	if (msg->unique_id.empty()) {
		pstmt->setNull(15, sql::DataType::VARCHAR);
	}
	else {
		pstmt->setString(15, msg->unique_id);
	}
	pstmt->setUInt64(16, msg->content_size);
	// content_hash/mime_type：资源消息必填，文本为空串时写 NULL
	if (msg->content_hash.empty()) {
		pstmt->setNull(17, sql::DataType::CHAR);
	}
	else {
		pstmt->setString(17, msg->content_hash);
	}
	if (msg->mime_type.empty()) {
		pstmt->setNull(18, sql::DataType::VARCHAR);
	}
	else {
		pstmt->setString(18, msg->mime_type);
	}

	int affected = pstmt->executeUpdate();

	// canonical message_id：新插入返回自增id，命中唯一键返回 LAST_INSERT_ID(expr) 设置的既有id
	std::unique_ptr<sql::Statement> keyStmt(conn->createStatement());
	std::unique_ptr<sql::ResultSet> rs(keyStmt->executeQuery("SELECT LAST_INSERT_ID()"));
	if (!rs->next()) {
		return SaveMessageResult::Failed;
	}
	msg->message_id = static_cast<std::int64_t>(rs->getUInt64(1));

	// 无条件按 canonical message_id 回读核对冲突字段，并恢复持久化状态。
	// 行不存在→Failed；业务字段全等才是同一消息；否则→Conflict。
	auto readStmt = std::unique_ptr<sql::PreparedStatement>(
		conn->prepareStatement((std::string("SELECT ") + kMessageColumns +
			" FROM chat_message WHERE message_id = ?").c_str())
	);
	readStmt->setUInt64(1, msg->message_id);
	std::unique_ptr<sql::ResultSet> rr(readStmt->executeQuery());
	if (!rr->next()) {
		return SaveMessageResult::Failed;
	}

	//资源消息（msg_type 1/3）比对 content_hash/mime_type；文本两列为 NULL 视为空串
	const std::string stored_hash = rr->isNull("content_hash") ? "" : rr->getString("content_hash");
	const std::string stored_mime = rr->isNull("mime_type") ? "" : rr->getString("mime_type");
	const std::string stored_unique = rr->isNull("unique_id") ? "" : rr->getString("unique_id");

	bool same =
		(rr->isNull("thread_id") ? 0 : static_cast<std::int64_t>(rr->getUInt64("thread_id"))) == msg->thread_id &&
		rr->getInt("sender_id") == msg->sender_id &&
		static_cast<int>(rr->getUInt64("recv_id")) == msg->recv_id &&
		stored_unique == msg->unique_id &&
		rr->getString("content") == msg->content &&
		rr->getInt("msg_type") == msg->msg_type &&
		rr->getUInt64("content_size") == msg->content_size &&
		stored_hash == msg->content_hash &&
		stored_mime == msg->mime_type;

	if (same) {
		// Duplicate 必须带回数据库真值；已就绪/过期的资源不能被默认值覆盖。
		const std::uint64_t stored_recv_seq =
			rr->isNull("recv_seq") ? 0 : rr->getUInt64("recv_seq");
		// 非资源消息已经在本事务拿到了大于当前序号头的新序号。只有实际插入
		// 的行才可能持有该序号，不依赖连接器是否启用 CLIENT_FOUND_ROWS。
		const bool inserted = requested_recv_seq > 0
			? stored_recv_seq == requested_recv_seq
			: affected == 1;
		msg->status = rr->getInt("status");
		msg->resource_status = static_cast<ResourceStatus>(rr->getInt("resource_status"));
		msg->content_hash = stored_hash;
		msg->mime_type = stored_mime;
		msg->chat_time = rr->getString("created_at");
		msg->recv_seq = stored_recv_seq;
		return inserted ? SaveMessageResult::Stored : SaveMessageResult::Duplicate;
	}
	out_conflict_uid = msg->unique_id;
	return SaveMessageResult::Conflict;
}

SaveMessageResult MysqlDao::AddChatMsg(std::shared_ptr<ChatMessage> chat_data) {
	auto con = pool_->getConnection();
	if (!con) {
		return SaveMessageResult::Failed;
	}
	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});
	auto& conn = con->_con;

	try {
		conn->setAutoCommit(false);
		const bool is_resource =
			chat_data->msg_type == static_cast<int>(ChatMsgType::PIC) ||
			chat_data->msg_type == static_cast<int>(ChatMsgType::FILE);
		if (!is_resource && !AllocateRecvSeq(conn.get(), chat_data->recv_id,
			chat_data->recv_seq)) {
			conn->rollback();
			return SaveMessageResult::Failed;
		}
		std::string conflict_uid;
		auto r = UpsertChatMessage(conn.get(), chat_data, conflict_uid);
		if (r == SaveMessageResult::Failed) {
			conn->rollback();
			return SaveMessageResult::Failed;
		}
		// Duplicate/Conflict 时撤销本次临时分配的 recv_seq；canonical 行保持不变。
		if (r == SaveMessageResult::Stored) conn->commit();
		else conn->rollback();
		return r;
	}
	catch (sql::SQLException& e) {
		std::cerr << "SQLException: " << e.what() << std::endl;
		conn->rollback();
		return SaveMessageResult::Failed;
	}
}

bool MysqlDao::GetMessagesAfterRecvSeq(int uid, std::uint64_t after_recv_seq,
	int limit, std::vector<SyncedMessage>& messages) {
	messages.clear();
	auto con = pool_->getConnection();
	if (!con) return false;
	Defer defer([this, &con]() { pool_->returnConnection(std::move(con)); });
	try {
		auto stmt = std::unique_ptr<sql::PreparedStatement>(con->_con->prepareStatement(
			(std::string("SELECT ") + kMessageColumns +
			 " FROM chat_message WHERE recv_id = ? AND recv_seq > ? "
			 "ORDER BY recv_seq ASC LIMIT ?").c_str()));
		stmt->setInt(1, uid);
		stmt->setUInt64(2, after_recv_seq);
		stmt->setInt(3, limit + 1);
		auto rs = std::unique_ptr<sql::ResultSet>(stmt->executeQuery());
		while (rs->next()) {
			auto msg = ReadMessage(rs.get());
			messages.push_back(SyncedMessage{msg->recv_seq, std::move(msg)});
		}
		return true;
	}
	catch (const sql::SQLException& e) {
		std::cerr << "GetMessagesAfterRecvSeq SQLException: " << e.what() << std::endl;
		messages.clear();
		return false;
	}
}

bool MysqlDao::GetLastRecvSeq(int uid, std::uint64_t& last_seq) {
	last_seq = 0;
	auto con = pool_->getConnection();
	if (!con) return false;
	Defer defer([this, &con]() { pool_->returnConnection(std::move(con)); });
	try {
		auto stmt = std::unique_ptr<sql::PreparedStatement>(con->_con->prepareStatement(
			"SELECT last_recv_seq FROM user WHERE uid = ?"));
		stmt->setInt(1, uid);
		auto rs = std::unique_ptr<sql::ResultSet>(stmt->executeQuery());
		if (!rs->next()) return false;
		last_seq = rs->getUInt64("last_recv_seq");
		return true;
	}
	catch (const sql::SQLException& e) {
		std::cerr << "GetLastRecvSeq SQLException: " << e.what() << std::endl;
		return false;
	}
}

std::shared_ptr<ChatMessage> MysqlDao::GetChatMsgById(std::int64_t message_id) {
	auto con = pool_->getConnection();
	if (!con) return nullptr;
	Defer defer([this, &con]() { pool_->returnConnection(std::move(con)); });
	try {
		auto stmt = std::unique_ptr<sql::PreparedStatement>(con->_con->prepareStatement(
			(std::string("SELECT ") + kMessageColumns +
			 " FROM chat_message WHERE message_id = ?").c_str()));
		stmt->setInt64(1, message_id);
		auto rs = std::unique_ptr<sql::ResultSet>(stmt->executeQuery());
		return rs->next() ? ReadMessage(rs.get()) : nullptr;
	}
	catch (const sql::SQLException& e) {
		std::cerr << "GetChatMsgById SQLException: " << e.what() << std::endl;
		return nullptr;
	}
}

