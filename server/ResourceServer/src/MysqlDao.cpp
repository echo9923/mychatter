#include "MysqlDao.h"
#include "ConfigMgr.h"
#include "const.h"
#include "PasswordHash.h"

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

bool MysqlDao::UpdateHeadInfo(int uid, const std::string& icon)
{
	auto con = pool_->getConnection();
	if (!con) {
		return false;
	}
	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});

	auto& conn = con->_con;
	try {
		std::string update_sql =
			"UPDATE user SET icon = ? WHERE uid = ?;";

		std::unique_ptr<sql::PreparedStatement> pstmt(conn->prepareStatement(update_sql));
		pstmt->setString(1, icon);
		pstmt->setInt64(2, uid);

		int affected_rows = pstmt->executeUpdate();

		// 检查是否有行被更新（可选）
		if (affected_rows == 0) {
			std::cerr << "No user found with uid: " << uid << std::endl;
			return false;
		}

		return true;
	}
	catch (sql::SQLException& e) {
		std::cerr << "SQLException in UpdateHeadInfo: " << e.what() << std::endl;
		return false;
	}
	return false;
}

bool MysqlDao::CompleteResourceUpload(long long chat_message_id, int sender_id, int recv_id,
	unsigned long long& recv_seq) {
	recv_seq = 0;
	auto con = pool_->getConnection();
	if (!con) return false;
	Defer defer([this, &con]() { pool_->returnConnection(std::move(con)); });
	auto& conn = con->_con;
	try {
		conn->setAutoCommit(false);
		auto read = std::unique_ptr<sql::PreparedStatement>(conn->prepareStatement(
			"SELECT sender_id, recv_id, resource_status, recv_seq FROM chat_message "
			"WHERE message_id = ? FOR UPDATE"));
		read->setInt64(1, chat_message_id);
		auto rs = std::unique_ptr<sql::ResultSet>(read->executeQuery());
		if (!rs->next() || rs->getInt("sender_id") != sender_id ||
			rs->getInt("recv_id") != recv_id) {
			conn->rollback();
			return false;
		}
		const int status = rs->getInt("resource_status");
		if (status == 1 && !rs->isNull("recv_seq")) {
			recv_seq = rs->getUInt64("recv_seq");
			conn->commit();
			return true;
		}
		if (status != 0) {
			conn->rollback();
			return false;
		}

		auto seq_stmt = std::unique_ptr<sql::PreparedStatement>(conn->prepareStatement(
			"SELECT last_recv_seq FROM user WHERE uid = ? FOR UPDATE"));
		seq_stmt->setInt(1, recv_id);
		auto seq_rs = std::unique_ptr<sql::ResultSet>(seq_stmt->executeQuery());
		if (!seq_rs->next()) {
			conn->rollback();
			return false;
		}
		recv_seq = seq_rs->getUInt64("last_recv_seq") + 1;
		auto update_user = std::unique_ptr<sql::PreparedStatement>(conn->prepareStatement(
			"UPDATE user SET last_recv_seq = ? WHERE uid = ?"));
		update_user->setUInt64(1, recv_seq);
		update_user->setInt(2, recv_id);
		if (update_user->executeUpdate() != 1) {
			conn->rollback();
			return false;
		}
		auto publish = std::unique_ptr<sql::PreparedStatement>(conn->prepareStatement(
			"UPDATE chat_message SET resource_status = 1, recv_seq = ? "
			"WHERE message_id = ? AND resource_status = 0 AND recv_seq IS NULL"));
		publish->setUInt64(1, recv_seq);
		publish->setInt64(2, chat_message_id);
		if (publish->executeUpdate() != 1) {
			conn->rollback();
			return false;
		}
		conn->commit();
		return true;
	}
	catch (const sql::SQLException& e) {
		conn->rollback();
		std::cerr << "CompleteResourceUpload SQLException: " << e.what() << std::endl;
		return false;
	}
}

std::shared_ptr<ChatMessage> MysqlDao::GetChatMsgById(long long message_id) {
	auto con = pool_->getConnection();
	if (!con) {
		return nullptr;
	}

	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});

	auto& conn = con->_con;

	try {
		auto pstmt = std::unique_ptr<sql::PreparedStatement>(
			conn->prepareStatement(
				"SELECT message_id, thread_id, recv_seq, sender_id, recv_id, "
				"content, created_at, updated_at, status, msg_type, resource_status, "
				"content_size, content_hash, mime_type, business_status, "
				"related_message_id, handled_at, requester_remark "
				"FROM chat_message WHERE message_id = ?"
			)
			);

		pstmt->setUInt64(1, message_id);
		auto rs = std::unique_ptr<sql::ResultSet>(pstmt->executeQuery());

		if (rs->next()) {
			auto msg = std::make_shared<ChatMessage>();
			msg->message_id = rs->getUInt64("message_id");
			msg->thread_id = rs->isNull("thread_id") ? 0 : rs->getUInt64("thread_id");
			msg->recv_seq = rs->isNull("recv_seq") ? 0 : rs->getUInt64("recv_seq");
			msg->sender_id = rs->getUInt64("sender_id");
			msg->recv_id = rs->getUInt64("recv_id");
			msg->content = rs->getString("content");
			msg->chat_time = rs->getString("created_at");
			msg->status = rs->getInt("status");
			msg->msg_type = rs->getInt("msg_type");
			msg->resource_status = rs->getInt("resource_status");
			msg->content_size = rs->getUInt64("content_size");
			msg->content_hash = rs->isNull("content_hash") ? "" : rs->getString("content_hash");
			msg->mime_type = rs->isNull("mime_type") ? "" : rs->getString("mime_type");
			msg->business_status = rs->getInt("business_status");
			msg->related_message_id = rs->isNull("related_message_id") ? 0 : rs->getInt64("related_message_id");
			msg->handled_at = rs->isNull("handled_at") ? "" : rs->getString("handled_at");
			msg->requester_remark = rs->isNull("requester_remark") ? "" : rs->getString("requester_remark");
			return msg;
		}

		return nullptr;

	}
	catch (sql::SQLException& e) {
		std::cerr << "GetChatMessageById SQLException: " << e.what() << std::endl;
		return nullptr;
	}
}

bool MysqlDao::GetExpiredResourceIds(const std::string& before_time, int limit,
	std::vector<ExpiredResource>& out) {
	out.clear();
	auto con = pool_->getConnection();
	if (!con) {
		return false;
	}
	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});

	auto& conn = con->_con;
	try {
		//仅看 resource_status=0（待上传）：Ready 资源不清理，Expired 无需重复标记
		auto pstmt = std::unique_ptr<sql::PreparedStatement>(
			conn->prepareStatement(
				"SELECT message_id, sender_id, recv_id FROM chat_message "
				"WHERE resource_status = 0 AND msg_type IN (1, 3) AND updated_at < ? "
				"ORDER BY updated_at ASC LIMIT ?"
			)
			);
		pstmt->setString(1, before_time);
		pstmt->setInt(2, limit);
		auto rs = std::unique_ptr<sql::ResultSet>(pstmt->executeQuery());
		while (rs->next()) {
			ExpiredResource item;
			item.message_id = rs->getUInt64("message_id");
			item.sender_id = rs->getUInt64("sender_id");
			item.recv_id = rs->getUInt64("recv_id");
			out.push_back(item);
		}
		return true;
	}
	catch (sql::SQLException& e) {
		std::cerr << "GetExpiredResourceIds SQLException: " << e.what() << std::endl;
		out.clear();
		return false;
	}
}

bool MysqlDao::MarkResourceExpired(const std::vector<ExpiredResource>& items) {
	if (items.empty()) {
		return true;
	}
	auto con = pool_->getConnection();
	if (!con) {
		return false;
	}
	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});

	auto& conn = con->_con;
	try {
		conn->setAutoCommit(false);
		// 未发布资源没有 recv_seq，过期只更新权威资源状态，不通知接收者。
		auto upd = std::unique_ptr<sql::PreparedStatement>(conn->prepareStatement(
			"UPDATE chat_message SET resource_status = 2 "
			"WHERE message_id = ? AND resource_status = 0"));
		for (const auto& item : items) {
			upd->setInt64(1, item.message_id);
			upd->executeUpdate();
		}
		conn->commit();
		return true;
	}
	catch (sql::SQLException& e) {
		std::cerr << "MarkResourceExpired SQLException: " << e.what() << std::endl;
		conn->rollback();
		return false;
	}
}
