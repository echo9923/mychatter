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

bool MysqlDao::CompleteResourceUploadWithSync(long long chat_message_id, int sender_id, int recv_id)
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
		// 状态迁移与同步行写入同生共死：显式事务（连接池存在 autocommit 残留，必须显式关闭）
		conn->setAutoCommit(false);

		//0→1 条件更新：已 Ready（重发末片再触发完成）不重复迁移，回读判幂等
		std::string update_sql =
			"UPDATE chat_message SET resource_status = 1 "
			"WHERE message_id = ? AND resource_status = 0;";

		std::unique_ptr<sql::PreparedStatement> pstmt(conn->prepareStatement(update_sql));
		pstmt->setInt64(1, chat_message_id);

		int affected_rows = pstmt->executeUpdate();

		if (affected_rows == 0) {
			//回读区分幂等成功（已 Ready）与真值缺失/终态
			std::unique_ptr<sql::PreparedStatement> read_stmt(conn->prepareStatement(
				"SELECT resource_status FROM chat_message WHERE message_id = ?"));
			read_stmt->setInt64(1, chat_message_id);
			std::unique_ptr<sql::ResultSet> rs(read_stmt->executeQuery());
			if (rs->next() && rs->getInt("resource_status")
				== static_cast<int>(ResourceStatus::Ready)) {
				//已就绪：补同步行后按成功提交（INSERT IGNORE 幂等）
			}
			else {
				conn->rollback();
				std::cerr << "CompleteResourceUploadWithSync: message " << chat_message_id
					<< " not found or not uploadable" << std::endl;
				return false;
			}
		}

		// 上传完成点才补双方同步行（此前 Uploading 资源对增量同步不可见）；
		// INSERT IGNORE 使续传重复完成天然幂等
		std::unique_ptr<sql::PreparedStatement> sync_stmt(conn->prepareStatement(
			"INSERT IGNORE INTO user_message_sync (uid, message_id) VALUES (?, ?), (?, ?)"
		));
		sync_stmt->setInt(1, sender_id);
		sync_stmt->setInt64(2, chat_message_id);
		sync_stmt->setInt(3, recv_id);
		sync_stmt->setInt64(4, chat_message_id);
		sync_stmt->executeUpdate();

		conn->commit();
		return true;
	}
	catch (sql::SQLException& e) {
		std::cerr << "SQLException in CompleteResourceUploadWithSync: " << e.what() << std::endl;
		conn->rollback();
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
				"SELECT message_id, thread_id, sender_id, recv_id, "
				"content, created_at, updated_at, status, msg_type, resource_status, "
				"content_size, content_hash, mime_type "
				"FROM chat_message WHERE message_id = ?"
			)
			);

		pstmt->setUInt64(1, message_id);
		auto rs = std::unique_ptr<sql::ResultSet>(pstmt->executeQuery());

		if (rs->next()) {
			auto msg = std::make_shared<ChatMessage>();
			msg->message_id = rs->getUInt64("message_id");
			msg->thread_id = rs->getUInt64("thread_id");
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
		//逐条条件更新 0→2 并补双方同步行：失败终态也必须进同步流，
		//客户端才能把气泡置为“已过期”而不是无限重试下载
		auto upd = std::unique_ptr<sql::PreparedStatement>(conn->prepareStatement(
			"UPDATE chat_message SET resource_status = 2 "
			"WHERE message_id = ? AND resource_status = 0"));
		auto sync_stmt = std::unique_ptr<sql::PreparedStatement>(conn->prepareStatement(
			"INSERT IGNORE INTO user_message_sync (uid, message_id) VALUES (?, ?), (?, ?)"));
		for (const auto& item : items) {
			upd->setInt64(1, item.message_id);
			upd->executeUpdate();
			sync_stmt->setInt(1, item.sender_id);
			sync_stmt->setInt64(2, item.message_id);
			sync_stmt->setInt(3, item.recv_id);
			sync_stmt->setInt64(4, item.message_id);
			sync_stmt->executeUpdate();
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
