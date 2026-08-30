#include "MysqlDao.h"
#include "ConfigMgr.h"
#include "PasswordHash.h"

MysqlDao::MysqlDao()
{
	auto & cfg = ConfigMgr::Inst();
	const auto& host = cfg["Mysql"]["Host"];
	const auto& port = cfg["Mysql"]["Port"];
	const auto& pwd = cfg["Mysql"]["Passwd"];
	const auto& schema = cfg["Mysql"]["Schema"];
	const auto& user = cfg["Mysql"]["User"];
	pool_.reset(new MySqlPool(host+":"+port, user, pwd,schema, 5));
}

MysqlDao::~MysqlDao(){
	pool_->Close();
}

int MysqlDao::RegUser(const std::string& name, const std::string& email, const std::string& pwd)
{
	return RegUserTransaction(name, email, pwd, std::string());
}

int MysqlDao::RegUserTransaction(const std::string& name, const std::string& email, const std::string& pwd, 
	const std::string& icon)
{
	auto con = pool_->getConnection();
	if (con == nullptr) {
		return false;
	}

	Defer defer([this, &con] {
		pool_->returnConnection(std::move(con));
	});

	try {
		//开始事务
		con->_con->setAutoCommit(false);
		//执行第一个数据库操作，根据email查找用户
			// 准备查询语句

		std::unique_ptr<sql::PreparedStatement> pstmt_email(con->_con->prepareStatement("SELECT 1 FROM users WHERE email = ?"));

		// 绑定参数
		pstmt_email->setString(1, email);

		// 执行查询
		std::unique_ptr<sql::ResultSet> res_email(pstmt_email->executeQuery());

		auto email_exist = res_email->next();
		if (email_exist) {
			con->_con->rollback();
			std::cout << "email " << email << " exist";
			return 0;
		}

		// 准备查询用户名是否重复
		std::unique_ptr<sql::PreparedStatement> pstmt_name(con->_con->prepareStatement("SELECT 1 FROM users WHERE username = ?"));

		// 绑定参数
		pstmt_name->setString(1, name);

		// 执行查询
		std::unique_ptr<sql::ResultSet> res_name(pstmt_name->executeQuery());

		auto name_exist = res_name->next();
		if (name_exist) {
			con->_con->rollback();
			std::cout << "name " << name << " exist";
			return 0;
		}

		// 密码先做 PBKDF2 哈希，DB 只存哈希
		const std::string hashed = llfc::HashPassword(pwd);
		if (hashed.empty()) {
			con->_con->rollback();
			return -1;
		}
		std::unique_ptr<sql::PreparedStatement> pstmt_insert(con->_con->prepareStatement("INSERT INTO users (username, email, password_hash, nickname, avatar_key) "
			"VALUES (?, ?, ?, ?, ?)"));
		pstmt_insert->setString(1, name);
		pstmt_insert->setString(2, email);
		pstmt_insert->setString(3, hashed);
		pstmt_insert->setString(4, name);
		pstmt_insert->setString(5, icon);
		//执行插入
		pstmt_insert->executeUpdate();

		// 获取本次插入的自增主键 id（同一连接、同一事务内，LAST_INSERT_ID() 安全）
		std::unique_ptr<sql::PreparedStatement> pstmt_lastid(con->_con->prepareStatement("SELECT LAST_INSERT_ID() AS id"));
		std::unique_ptr<sql::ResultSet> res_lastid(pstmt_lastid->executeQuery());
		int newId = 0;
		if (res_lastid->next()) {
			newId = res_lastid->getInt("id");
		}
		else {
			std::cout << "select LAST_INSERT_ID failed" << std::endl;
			con->_con->rollback();
			return -1;
		}

		// 提交事务
		con->_con->commit();
		std::cout << "new user inserted into users, user_id=" << newId << std::endl;
		return newId;
	}
	catch (sql::SQLException& e) {
		// 如果发生错误，回滚事务
		if (con) {
			con->_con->rollback();
		}
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
		std::unique_ptr<sql::PreparedStatement> pstmt(con->_con->prepareStatement("SELECT email FROM users WHERE username = ?"));

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
		return false;
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
		std::unique_ptr<sql::PreparedStatement> pstmt(con->_con->prepareStatement("UPDATE users SET password_hash = ? WHERE username = ?"));

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

bool MysqlDao::CheckPwd(const std::string& email, const std::string& pwd, UserInfo& userInfo) {
	auto con = pool_->getConnection();
	if (con == nullptr) {
		return false;
	}

	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});

	try {
	

		// 准备SQL语句
		std::unique_ptr<sql::PreparedStatement> pstmt(con->_con->prepareStatement(
			"SELECT user_id, username, email, password_hash FROM users WHERE email = ?"));
		pstmt->setString(1, email); // 将username替换为你要查询的用户名

		// 执行查询
		std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
		std::string origin_pwd = "";
		// 遍历结果集
		while (res->next()) {
			origin_pwd = res->getString("password_hash");
			userInfo.name = res->getString("username");
			userInfo.email = res->getString("email");
			userInfo.uid = res->getInt("user_id");
			break;
		}

		// PBKDF2 恒定时间校验；pwd 必须为 pbkdf2-sha256 格式，明文/畸形值一律拒绝
		if (!llfc::VerifyPassword(pwd, origin_pwd)) {
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
}

bool MysqlDao::TestProcedure(const std::string& email, int& uid, string& name) {
	auto con = pool_->getConnection();
	try {
		if (con == nullptr) {
			return false;
		}

		Defer defer([this, &con]() {
			pool_->returnConnection(std::move(con));
			});
		unique_ptr<sql::PreparedStatement> stmt(con->_con->prepareStatement(
			"SELECT user_id, username FROM users WHERE email = ?"));
		stmt->setString(1, email);
		unique_ptr<sql::ResultSet> res(stmt->executeQuery());
		if (!(res->next())) {
			return false;
		}
		uid = res->getInt("user_id");
		name = res->getString("username");
		return true;

	}
	catch (sql::SQLException& e) {
		std::cerr << "SQLException: " << e.what();
		std::cerr << " (MySQL error code: " << e.getErrorCode();
		std::cerr << ", SQLState: " << e.getSQLState() << " )" << std::endl;
		return false;
	}
}
