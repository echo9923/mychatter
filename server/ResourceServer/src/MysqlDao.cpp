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

bool MysqlDao::UpdateUploadStatus(int chat_message_id)
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
		// 连接池中被 AddFriend 等方法留下 autocommit=false 的连接，必须显式恢复。
		conn->setAutoCommit(true);
		std::string update_sql =
			"UPDATE chat_message SET status = ? WHERE message_id = ?;";

		std::unique_ptr<sql::PreparedStatement> pstmt(conn->prepareStatement(update_sql));
		pstmt->setInt(1, MsgStatus::READED);
		pstmt->setInt64(2, chat_message_id);

		int affected_rows = pstmt->executeUpdate();

		// 检查是否有行被更新（可选）
		if (affected_rows == 0) {
			std::cerr << "No chat message found with chat_message_id: " << chat_message_id << std::endl;
			return false;
		}

		return true;
	}
	catch (sql::SQLException& e) {
		std::cerr << "SQLException in UpdateUploadStatus: " << e.what() << std::endl;
		return false;
	}
	return false;
}

std::shared_ptr<ChatMessage> MysqlDao::GetChatMsgById(int message_id) {
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
				"content, created_at, updated_at, status , msg_type "
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
			return msg;
		}

		return nullptr;

	}
	catch (sql::SQLException& e) {
		std::cerr << "GetChatMessageById SQLException: " << e.what() << std::endl;
		return nullptr;
	}
}
