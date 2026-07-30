#include "MysqlDao.h"
#include "ConfigMgr.h"

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

int MysqlDao::RegUser(const std::string& name, const std::string& email, const std::string& pwd)
{
	auto con = pool_->getConnection();
	try {
		if (con == nullptr) {
			return false;
		}
		// 准备调用存储过程
		std::unique_ptr < sql::PreparedStatement > stmt(con->_con->prepareStatement("CALL reg_user(?,?,?,@result)"));
		// 设置输入参数
		stmt->setString(1, name);
		stmt->setString(2, email);
		stmt->setString(3, pwd);

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

		// 准备查询语句
		std::unique_ptr<sql::PreparedStatement> pstmt(con->_con->prepareStatement("UPDATE user SET pwd = ? WHERE name = ?"));

		// 绑定参数
		pstmt->setString(2, name);
		pstmt->setString(1, newpwd);

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
			// 输出查询到的密码
			std::cout << "Password: " << origin_pwd << std::endl;
			break;
		}

		if (pwd != origin_pwd) {
			return false;
		}
		userInfo.name = name;
		userInfo.email = res->getString("email");
		userInfo.uid = res->getInt("uid");
		userInfo.pwd = origin_pwd;
		return true;
	}
	catch (sql::SQLException& e) {
		std::cerr << "SQLException: " << e.what();
		std::cerr << " (MySQL error code: " << e.getErrorCode();
		std::cerr << ", SQLState: " << e.getSQLState() << " )" << std::endl;
		return false;
	}
}

bool MysqlDao::AddFriendApply(const int& from, const int& to,
	const std::string& desc, const std::string& back_name)
{
	auto con = pool_->getConnection();
	if (con == nullptr) {
		return false;
	}

	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});

	try {
		// 准备SQL语句
		std::unique_ptr<sql::PreparedStatement> pstmt(
			con->_con->prepareStatement("INSERT INTO friend_apply (from_uid, to_uid, descs, back_name) "
				"values (?,?,?,?) "
				"ON DUPLICATE KEY UPDATE from_uid = from_uid, to_uid = to_uid, descs = ?, back_name = ?"));
		pstmt->setInt(1, from); // from id
		pstmt->setInt(2, to);
		pstmt->setString(3, desc);
		pstmt->setString(4, back_name);
		pstmt->setString(5, desc);
		pstmt->setString(6, back_name);
		// 执行更新
		int rowAffected = pstmt->executeUpdate();
		if (rowAffected < 0) {
			return false;
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

bool MysqlDao::AuthFriendApply(const int& from, const int& to) {
	auto con = pool_->getConnection();
	if (con == nullptr) {
		return false;
	}

	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});

	try {
		// 准备SQL语句
		std::unique_ptr<sql::PreparedStatement> pstmt(con->_con->prepareStatement("UPDATE friend_apply SET status = 1 "
			"WHERE from_uid = ? AND to_uid = ?"));
		//反过来的申请时from，验证时to
		pstmt->setInt(1, to); // from id
		pstmt->setInt(2, from);
		// 执行更新
		int rowAffected = pstmt->executeUpdate();
		if (rowAffected < 0) {
			return false;
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


bool MysqlDao::AddFriend(const int& from, const int& to, std::string back_name,
	std::vector<std::shared_ptr<AddFriendMsg>>& chat_datas) {
	auto con = pool_->getConnection();
	if (con == nullptr) {
		return false;
	}

	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});

	try {
		// 开始事务
		con->_con->setAutoCommit(false);
		std::string reverse_back;
		std::string apply_desc;

		{
			// 1. 锁定并读取
			std::unique_ptr<sql::PreparedStatement> selStmt(con->_con->prepareStatement(
				"SELECT back_name, descs "
				"FROM friend_apply "
				"WHERE from_uid = ? AND to_uid = ? "
				"FOR UPDATE"
			));
			selStmt->setInt(1, to);
			selStmt->setInt(2, from);

			std::unique_ptr<sql::ResultSet> rsSel(selStmt->executeQuery());

			if (rsSel->next()) {
				reverse_back = rsSel->getString("back_name");
				apply_desc = rsSel->getString("descs");
			}
			else {
				// 没有对应的申请记录，直接 rollback 并返回失败
				con->_con->rollback();
				return false;
			}
		}

		{
			// 2. 执行真正的更新
			std::unique_ptr<sql::PreparedStatement> updStmt(con->_con->prepareStatement(
				"UPDATE friend_apply "
				"SET status = 1 "
				"WHERE from_uid = ? AND to_uid = ?"
			));

			updStmt->setInt(1, to);
			updStmt->setInt(2, from);

			if (updStmt->executeUpdate() != 1) {
				// 更新行数不对，回滚
				con->_con->rollback();
				return false;
			}
		}

		{
			// 3. 插入好友关系 - 关键改进：按照固定顺序插入避免死锁
			// 确定插入顺序：始终按照 uid 大小顺序
			int smaller_uid = std::min(from, to);
			int larger_uid = std::max(from, to);

			// 第一次插入：较小的 uid 作为 self_id
			std::unique_ptr<sql::PreparedStatement> pstmt(con->_con->prepareStatement(
				"INSERT IGNORE INTO friend(self_id, friend_id, back) "
				"VALUES (?, ?, ?)"
			));

			if (from == smaller_uid) {
				pstmt->setInt(1, from);
				pstmt->setInt(2, to);
				pstmt->setString(3, back_name);
			}
			else {
				pstmt->setInt(1, to);
				pstmt->setInt(2, from);
				pstmt->setString(3, reverse_back);
			}

			int rowAffected = pstmt->executeUpdate();
			if (rowAffected < 0) {
				con->_con->rollback();
				return false;
			}

			// 第二次插入：较大的 uid 作为 self_id
			std::unique_ptr<sql::PreparedStatement> pstmt2(con->_con->prepareStatement(
				"INSERT IGNORE INTO friend(self_id, friend_id, back) "
				"VALUES (?, ?, ?)"
			));

			if (from == larger_uid) {
				pstmt2->setInt(1, from);
				pstmt2->setInt(2, to);
				pstmt2->setString(3, back_name);
			}
			else {
				pstmt2->setInt(1, to);
				pstmt2->setInt(2, from);
				pstmt2->setString(3, reverse_back);
			}

			int rowAffected2 = pstmt2->executeUpdate();
			if (rowAffected2 < 0) {
				con->_con->rollback();
				return false;
			}
		}

		// 4. 创建 chat_thread
		long long threadId = 0;
		{
			std::unique_ptr<sql::PreparedStatement> threadStmt(con->_con->prepareStatement(
				"INSERT INTO chat_thread (type, created_at) VALUES ('private', NOW())"
			));

			threadStmt->executeUpdate();

			std::unique_ptr<sql::Statement> stmt(con->_con->createStatement());
			std::unique_ptr<sql::ResultSet> rs(
				stmt->executeQuery("SELECT LAST_INSERT_ID()")
			);

			if (rs->next()) {
				threadId = rs->getInt64(1);
			}
			else {
				con->_con->rollback();
				return false;
			}
		}

		// 5. 插入 private_chat
		{
			std::unique_ptr<sql::PreparedStatement> pcStmt(con->_con->prepareStatement(
				"INSERT INTO private_chat(thread_id, user1_id, user2_id) VALUES (?, ?, ?)"
			));

			pcStmt->setInt64(1, threadId);
			pcStmt->setInt(2, from);
			pcStmt->setInt(3, to);

			if (pcStmt->executeUpdate() < 0) {
				con->_con->rollback();
				return false;
			}
		}

		// 6. 插入初始消息（申请描述）
		if (!apply_desc.empty())
		{
			std::unique_ptr<sql::PreparedStatement> msgStmt(con->_con->prepareStatement(
			"INSERT INTO chat_message(thread_id, sender_id, recv_id, content, created_at, updated_at, status, msg_type, unique_id, content_size, delivery_status) "
			"VALUES (?, ?, ?, ?, NOW(), NOW(), ?, 0, NULL, 0, 1)"
		));

			msgStmt->setInt64(1, threadId);
			msgStmt->setInt(2, to);
			msgStmt->setInt(3, from);
			msgStmt->setString(4, apply_desc);
			msgStmt->setInt(5, 2);

			if (msgStmt->executeUpdate() < 0) {
				con->_con->rollback();
				return false;
			}

			std::unique_ptr<sql::Statement> stmt(con->_con->createStatement());
			std::unique_ptr<sql::ResultSet> rs(
				stmt->executeQuery("SELECT LAST_INSERT_ID()")
			);

			if (rs->next()) {
				auto messageId = rs->getInt64(1);
				auto tx_data = std::make_shared<AddFriendMsg>();
				tx_data->set_sender_id(to);
				tx_data->set_msg_id(messageId);
				tx_data->set_msgcontent(apply_desc);
				tx_data->set_thread_id(threadId);
				tx_data->set_unique_id("");
				tx_data->set_status(2);
				std::cout << "addfriend insert message success" << std::endl;
				chat_datas.push_back(tx_data);
			}
			else {
				con->_con->rollback();
				return false;
			}
		}

		// 7. 插入成为好友的消息
		{
			std::unique_ptr<sql::PreparedStatement> msgStmt(con->_con->prepareStatement(
			"INSERT INTO chat_message(thread_id, sender_id, recv_id, content, created_at, updated_at, status, msg_type, unique_id, content_size, delivery_status) "
			"VALUES (?, ?, ?, ?, NOW(), NOW(), ?, 0, NULL, 0, 1)"
		));

			msgStmt->setInt64(1, threadId);
			msgStmt->setInt(2, from);
			msgStmt->setInt(3, to);
			msgStmt->setString(4, "We are friends now!");
			msgStmt->setInt(5, 2);

			if (msgStmt->executeUpdate() < 0) {
				con->_con->rollback();
				return false;
			}

			std::unique_ptr<sql::Statement> stmt(con->_con->createStatement());
			std::unique_ptr<sql::ResultSet> rs(
				stmt->executeQuery("SELECT LAST_INSERT_ID()")
			);

			if (rs->next()) {
				auto messageId = rs->getInt64(1);
				auto tx_data = std::make_shared<AddFriendMsg>();
				tx_data->set_sender_id(from);
				tx_data->set_msg_id(messageId);
				tx_data->set_msgcontent("We are friends now!");
				tx_data->set_thread_id(threadId);
				tx_data->set_unique_id("");
				tx_data->set_status(2);
				chat_datas.push_back(tx_data);
			}
			else {
				con->_con->rollback();
				return false;
			}
		}

		// 提交事务
		con->_con->commit();
		std::cout << "addfriend insert friends success" << std::endl;

		return true;
	}
	catch (sql::SQLException& e) {
		// 如果发生错误，回滚事务
		if (con) {
			con->_con->rollback();
		}
		std::cerr << "SQLException: " << e.what();
		std::cerr << " (MySQL error code: " << e.getErrorCode();
		std::cerr << ", SQLState: " << e.getSQLState() << " )" << std::endl;

		// 如果是死锁错误（1213），可以考虑重试
		if (e.getErrorCode() == 1213) {
			std::cerr << "Deadlock detected, consider retry" << std::endl;
		}

		return false;
	}

	return true;
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
			user_ptr->pwd = res->getString("pwd");
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
			user_ptr->pwd = res->getString("pwd");
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


bool MysqlDao::GetApplyList(int touid, std::vector<std::shared_ptr<ApplyInfo>>& applyList, int begin, int limit) {
	auto con = pool_->getConnection();
	if (con == nullptr) {
		return false;
	}

	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});


	try {
		// 准备SQL语句, 根据起始id和限制条数返回列表
		std::unique_ptr<sql::PreparedStatement> pstmt(con->_con->prepareStatement("select apply.from_uid, apply.status, user.name, "
			"user.nick, user.sex from friend_apply as apply join user on apply.from_uid = user.uid where apply.to_uid = ? "
			"and apply.id > ? order by apply.id ASC LIMIT ? "));

		pstmt->setInt(1, touid); // 将uid替换为你要查询的uid
		pstmt->setInt(2, begin); // 起始id
		pstmt->setInt(3, limit); //偏移量
		// 执行查询
		std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
		// 遍历结果集
		while (res->next()) {
			auto name = res->getString("name");
			auto uid = res->getInt("from_uid");
			auto status = res->getInt("status");
			auto nick = res->getString("nick");
			auto sex = res->getInt("sex");
			auto apply_ptr = std::make_shared<ApplyInfo>(uid, name, "", "", nick, sex, status);
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
		// 准备SQL语句, 根据起始id和限制条数返回列表
		std::unique_ptr<sql::PreparedStatement> pstmt(con->_con->prepareStatement("select * from friend where self_id = ? "));

		pstmt->setInt(1, self_id); // 将uid替换为你要查询的uid

		// 执行查询
		std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
		// 遍历结果集
		while (res->next()) {
			auto friend_id = res->getInt("friend_id");
			auto back = res->getString("back");
			//再一次查询friend_id对应的信息
			auto user_info = GetUser(friend_id);
			if (user_info == nullptr) {
				continue;
			}

			user_info->back = user_info->name;
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
	int& nextLastId)
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
			"SELECT thread_id, type, user1_id, user2_id "
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

bool MysqlDao::CreatePrivateChat(int user1_id, int user2_id, int& thread_id)
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
			thread_id = res->getInt("thread_id");
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
		thread_id = res_last_id->getInt(1);

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
					thread_id = res_retry->getInt("thread_id");
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

std::shared_ptr<PageResult> MysqlDao::LoadChatMsg(int thread_id, int last_message_id, int page_size)
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
        SELECT message_id, thread_id, sender_id, recv_id, content,
               created_at, updated_at, status, msg_type,
               unique_id, content_size, delivery_status
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
		pstmt->setInt(1, thread_id);
		pstmt->setInt(2, last_message_id);
		pstmt->setInt(3, fetch_limit);

		auto rs = std::unique_ptr<sql::ResultSet>(pstmt->executeQuery());

		// 读取 fetch_limit 条记录
		while (rs->next()) {
			ChatMessage msg;
			msg.message_id = rs->getUInt64("message_id");
			msg.thread_id = rs->getUInt64("thread_id");
			msg.sender_id = rs->getUInt64("sender_id");
			msg.recv_id = rs->getUInt64("recv_id");
			msg.content = rs->getString("content");
			msg.chat_time = rs->getString("created_at");
			msg.status = rs->getInt("status");
			msg.msg_type = rs->getInt("msg_type");
			msg.unique_id = rs->getString("unique_id");
			msg.content_size = rs->getUInt64("content_size");
			msg.delivery_status = static_cast<DeliveryStatus>(rs->getInt("delivery_status"));
			page_res->messages.push_back(std::move(msg));
		}
		if (page_res->messages.size() > page_size) {
			page_res->messages.pop_back();
			page_res->load_more = true;
		}

		page_res->next_cursor = page_res->messages.back().message_id;

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

	// 幂等 UPSERT：命中 (sender_id, unique_id) 唯一键时，通过 LAST_INSERT_ID(expr)
	// 把 canonical message_id 暴露出来，且不改动原行任何业务字段（不得覆盖原消息）。
	auto pstmt = std::unique_ptr<sql::PreparedStatement>(
		conn->prepareStatement(
			"INSERT INTO chat_message "
			"(thread_id, sender_id, recv_id, content, created_at, updated_at, "
			" status, msg_type, unique_id, content_size, delivery_status) "
			"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
			"ON DUPLICATE KEY UPDATE message_id = LAST_INSERT_ID(message_id)"
		)
	);
	pstmt->setUInt64(1, msg->thread_id);
	pstmt->setUInt64(2, msg->sender_id);
	pstmt->setUInt64(3, msg->recv_id);
	pstmt->setString(4, msg->content);
	pstmt->setString(5, msg->chat_time);   // created_at
	pstmt->setString(6, msg->chat_time);   // updated_at
	pstmt->setInt(7, msg->status);
	pstmt->setInt(8, msg->msg_type);
	// 客户端消息 unique_id 非空；空串时写 NULL，避免同一 sender 的空串互相撞唯一键
	if (msg->unique_id.empty()) {
		pstmt->setNull(9, sql::DataType::VARCHAR);
	}
	else {
		pstmt->setString(9, msg->unique_id);
	}
	pstmt->setUInt64(10, msg->content_size);
	pstmt->setInt(11, static_cast<int>(msg->delivery_status));

	int affected = pstmt->executeUpdate();

	// canonical message_id：新插入返回自增id，命中唯一键返回 LAST_INSERT_ID(expr) 设置的既有id
	std::unique_ptr<sql::Statement> keyStmt(conn->createStatement());
	std::unique_ptr<sql::ResultSet> rs(keyStmt->executeQuery("SELECT LAST_INSERT_ID()"));
	if (!rs->next()) {
		return SaveMessageResult::Failed;
	}
	msg->message_id = static_cast<int>(rs->getUInt64(1));

	// 无条件按 canonical message_id 回读核对冲突字段，并恢复持久化状态。
	// 行不存在→Failed；五个业务字段全等才是同一消息；否则→Conflict。
	auto readStmt = std::unique_ptr<sql::PreparedStatement>(
		conn->prepareStatement(
			"SELECT thread_id, recv_id, content, msg_type, content_size, "
			"status, created_at, delivery_status "
			"FROM chat_message WHERE message_id = ?"
		)
	);
	readStmt->setUInt64(1, msg->message_id);
	std::unique_ptr<sql::ResultSet> rr(readStmt->executeQuery());
	if (!rr->next()) {
		return SaveMessageResult::Failed;
	}

	bool same =
		static_cast<int>(rr->getUInt64("thread_id")) == msg->thread_id &&
		static_cast<int>(rr->getUInt64("recv_id")) == msg->recv_id &&
		rr->getString("content") == msg->content &&
		rr->getInt("msg_type") == msg->msg_type &&
		rr->getUInt64("content_size") == msg->content_size;

	if (same) {
		// Duplicate 必须带回数据库真值：已 ACK 的消息不能因默认 Pending 被重新激活。
		msg->status = rr->getInt("status");
		msg->chat_time = rr->getString("created_at");
		msg->delivery_status = static_cast<DeliveryStatus>(rr->getInt("delivery_status"));
		return affected == 1 ? SaveMessageResult::Stored : SaveMessageResult::Duplicate;
	}
	out_conflict_uid = msg->unique_id;
	return SaveMessageResult::Conflict;
}

SaveMessageResult MysqlDao::AddChatMsg(std::vector<std::shared_ptr<ChatMessage>>& chat_datas,
	std::vector<std::string>& conflict_unique_ids) {
	conflict_unique_ids.clear();
	if (chat_datas.empty()) {
		return SaveMessageResult::Stored;
	}

	auto con = pool_->getConnection();
	if (!con) {
		return SaveMessageResult::Failed;
	}
	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
	});
	auto& conn = con->_con;

	try {
		// 批量在同一事务内处理；任一 conflict/SQL 错误回滚本批新行
		conn->setAutoCommit(false);

		bool any_conflict = false;
		bool all_duplicate = true;
		for (auto& msg : chat_datas) {
			std::string conflict_uid;
			auto r = UpsertChatMessage(conn.get(), msg, conflict_uid);
			if (r == SaveMessageResult::Failed) {
				conn->rollback();
				return SaveMessageResult::Failed;
			}
			if (r == SaveMessageResult::Conflict) {
				any_conflict = true;
				conflict_unique_ids.push_back(msg->unique_id);
				continue;
			}
			if (r == SaveMessageResult::Stored) {
				all_duplicate = false;
			}
			// Duplicate: 保持 all_duplicate=true
		}

		if (any_conflict) {
			// 存在内容冲突，回滚本批全部新行，交由上层返回 MESSAGE_CONFLICT
			conn->rollback();
			return SaveMessageResult::Conflict;
		}

		conn->commit();
		return all_duplicate ? SaveMessageResult::Duplicate : SaveMessageResult::Stored;
	}
	catch (sql::SQLException& e) {
		std::cerr << "SQLException: " << e.what() << std::endl;
		conn->rollback();
		return SaveMessageResult::Failed;
	}
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
		// 单条同样走幂等 UPSERT，自成事务
		conn->setAutoCommit(false);
		std::string conflict_uid;
		auto r = UpsertChatMessage(conn.get(), chat_data, conflict_uid);
		if (r == SaveMessageResult::Failed) {
			conn->rollback();
			return SaveMessageResult::Failed;
		}
		// Stored/Duplicate/Conflict 均未改动原行（Conflict 时 UPSERT 仅 set message_id=message_id）
		conn->commit();
		return r;
	}
	catch (sql::SQLException& e) {
		std::cerr << "SQLException: " << e.what() << std::endl;
		conn->rollback();
		return SaveMessageResult::Failed;
	}
}

std::shared_ptr<ChatMessage> MysqlDao::GetChatMsg(int message_id)
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
		auto pstmt = std::unique_ptr<sql::PreparedStatement>(
			conn->prepareStatement(
				"SELECT message_id, thread_id, sender_id, recv_id, "
				"content, created_at, updated_at, status, msg_type, "
				"unique_id, content_size, delivery_status "
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
			msg->unique_id = rs->getString("unique_id");
			msg->content_size = rs->getUInt64("content_size");
			msg->delivery_status = static_cast<DeliveryStatus>(rs->getInt("delivery_status"));

			return msg;
		}

		return nullptr;

	}
	catch (sql::SQLException& e) {
		std::cerr << "GetChatMessageById SQLException: " << e.what() << std::endl;
		return nullptr;
	}
}

std::vector<std::shared_ptr<ChatMessage>> MysqlDao::GetPendingMessages(int recv_uid,
	int after_message_id, int limit) {
	std::vector<std::shared_ptr<ChatMessage>> result;
	auto con = pool_->getConnection();
	if (!con) {
		return result;
	}
	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
	});
	auto& conn = con->_con;

	try {
		// delivery_status=0 的待投递消息；排除尚未上传完成的图片（msg_type=PIC && status=UN_UPLOAD）。
		// 多取一条供调用方判断 has_more，按 message_id 升序。
		auto pstmt = std::unique_ptr<sql::PreparedStatement>(
			conn->prepareStatement(
				"SELECT message_id, thread_id, sender_id, recv_id, content, "
				"created_at, updated_at, status, msg_type, unique_id, "
				"content_size, delivery_status "
				"FROM chat_message "
				"WHERE recv_id = ? AND delivery_status = 0 AND message_id > ? "
				"AND NOT (msg_type = 1 AND status = 3) "
				"ORDER BY message_id ASC LIMIT ?"
			)
		);
		pstmt->setInt(1, recv_uid);
		pstmt->setInt(2, after_message_id);
		pstmt->setInt(3, limit + 1);  // 多取一条供 has_more 判断

		auto rs = std::unique_ptr<sql::ResultSet>(pstmt->executeQuery());
		while (rs->next()) {
			auto msg = std::make_shared<ChatMessage>();
			msg->message_id = rs->getUInt64("message_id");
			msg->thread_id = rs->getUInt64("thread_id");
			msg->sender_id = rs->getUInt64("sender_id");
			msg->recv_id = rs->getUInt64("recv_id");
			msg->content = rs->getString("content");
			msg->chat_time = rs->getString("created_at");
			msg->status = rs->getInt("status");
			msg->msg_type = rs->getInt("msg_type");
			msg->unique_id = rs->getString("unique_id");
			msg->content_size = rs->getUInt64("content_size");
			msg->delivery_status = static_cast<DeliveryStatus>(rs->getInt("delivery_status"));
			result.push_back(msg);
		}
	}
	catch (sql::SQLException& e) {
		std::cerr << "GetPendingMessages SQLException: " << e.what() << std::endl;
		result.clear();
	}
	return result;
}

std::vector<std::shared_ptr<ChatMessage>> MysqlDao::GetMessagesByIds(int recv_uid,
	const std::vector<int>& ids) {
	std::vector<std::shared_ptr<ChatMessage>> result;
	if (ids.empty()) {
		return result;
	}
	auto con = pool_->getConnection();
	if (!con) {
		return result;
	}
	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
	});
	auto& conn = con->_con;

	try {
		// 按 recv_uid 过滤防越权；message_id 主键命中。
		std::string sql = "SELECT message_id, thread_id, sender_id, recv_id, content, "
			"created_at, updated_at, status, msg_type, unique_id, "
			"content_size, delivery_status "
			"FROM chat_message WHERE recv_id = ? AND message_id IN (";
		for (std::size_t i = 0; i < ids.size(); ++i) {
			sql += (i == 0 ? "?" : ",?");
		}
		sql += ")";

		auto pstmt = std::unique_ptr<sql::PreparedStatement>(conn->prepareStatement(sql));
		pstmt->setInt(1, recv_uid);
		for (std::size_t i = 0; i < ids.size(); ++i) {
			pstmt->setInt(static_cast<unsigned int>(2 + i), ids[i]);
		}

		auto rs = std::unique_ptr<sql::ResultSet>(pstmt->executeQuery());
		while (rs->next()) {
			auto msg = std::make_shared<ChatMessage>();
			msg->message_id = rs->getUInt64("message_id");
			msg->thread_id = rs->getUInt64("thread_id");
			msg->sender_id = rs->getUInt64("sender_id");
			msg->recv_id = rs->getUInt64("recv_id");
			msg->content = rs->getString("content");
			msg->chat_time = rs->getString("created_at");
			msg->status = rs->getInt("status");
			msg->msg_type = rs->getInt("msg_type");
			msg->unique_id = rs->getString("unique_id");
			msg->content_size = rs->getUInt64("content_size");
			msg->delivery_status = static_cast<DeliveryStatus>(rs->getInt("delivery_status"));
			result.push_back(msg);
		}
	}
	catch (sql::SQLException& e) {
		std::cerr << "GetMessagesByIds SQLException: " << e.what() << std::endl;
		result.clear();
	}
	return result;
}

bool MysqlDao::MarkMessagesDelivered(int recv_uid, const std::vector<int>& ids) {
	if (ids.empty()) {
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
		// UPDATE 带 recv_id 过滤防越权；重复 ACK（已为 1）仍返回成功（幂等）。
		std::string sql = "UPDATE chat_message SET delivery_status = 1 "
			"WHERE recv_id = ? AND delivery_status = 0 AND message_id IN (";
		for (std::size_t i = 0; i < ids.size(); ++i) {
			sql += (i == 0 ? "?" : ",?");
		}
		sql += ")";

		auto pstmt = std::unique_ptr<sql::PreparedStatement>(conn->prepareStatement(sql));
		pstmt->setInt(1, recv_uid);
		for (std::size_t i = 0; i < ids.size(); ++i) {
			pstmt->setInt(static_cast<unsigned int>(2 + i), ids[i]);
		}
		pstmt->executeUpdate();
		return true;
	}
	catch (sql::SQLException& e) {
		std::cerr << "MarkMessagesDelivered SQLException: " << e.what() << std::endl;
		return false;
	}
}

