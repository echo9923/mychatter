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
	auto con = pool_->getConnection();
	try {
		if (con == nullptr) {
			return false;
		}
		// 准备调用存储过程
		unique_ptr < sql::PreparedStatement > stmt(con->_con->prepareStatement("CALL reg_user(?,?,?,@result)"));
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
	   unique_ptr<sql::Statement> stmtResult(con->_con->createStatement());
	  unique_ptr<sql::ResultSet> res(stmtResult->executeQuery("SELECT @result AS result"));
	  if (res->next()) {
	       int result = res->getInt("result");
	      cout << "Result: " << result << endl;
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

		std::unique_ptr<sql::PreparedStatement> pstmt_email(con->_con->prepareStatement("SELECT 1 FROM user WHERE email = ?"));

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
		std::unique_ptr<sql::PreparedStatement> pstmt_name(con->_con->prepareStatement("SELECT 1 FROM user WHERE name = ?"));

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
		// 插入user信息（uid 列省略，走 DEFAULT 0，随后回填为自增主键 id）
		std::unique_ptr<sql::PreparedStatement> pstmt_insert(con->_con->prepareStatement("INSERT INTO user (name, email, pwd, nick, icon) "
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

		// 回填 uid = 自增主键 id（方案A：不再使用独立发号器，uid 直接等于 id）
		std::unique_ptr<sql::PreparedStatement> pstmt_setuid(con->_con->prepareStatement("UPDATE user SET uid = ? WHERE id = ?"));
		pstmt_setuid->setInt(1, newId);
		pstmt_setuid->setInt(2, newId);
		pstmt_setuid->executeUpdate();

		// 提交事务
		con->_con->commit();
		std::cout << "newuser insert into user success, uid = " << newId << std::endl;
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
		std::unique_ptr<sql::PreparedStatement> pstmt(con->_con->prepareStatement("SELECT * FROM user WHERE email = ?"));
		pstmt->setString(1, email); // 将username替换为你要查询的用户名

		// 执行查询
		std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
		std::string origin_pwd = "";
		// 遍历结果集
		while (res->next()) {
			origin_pwd = res->getString("pwd");
			userInfo.name = res->getString("name");
			userInfo.email = res->getString("email");
			userInfo.uid = res->getInt("uid");
			break;
		}

		// PBKDF2 恒定时间校验；存量明文行由 VerifyPassword 的 legacy 分支兼容，
		// 校验通过后由 ShouldRehash 触发 rehash-on-login，把明文平滑升级为哈希。
		if (!llfc::VerifyPassword(pwd, origin_pwd)) {
			return false;
		}
		if (llfc::ShouldRehash(origin_pwd)) {
			const std::string hashed = llfc::HashPassword(pwd);
			if (!hashed.empty()) {
				// 复用同一连接执行 UPDATE，避免二次向连接池取连接
				std::unique_ptr<sql::PreparedStatement> pstmt_upd(
					con->_con->prepareStatement("UPDATE user SET pwd = ? WHERE name = ?"));
				pstmt_upd->setString(1, hashed);
				pstmt_upd->setString(2, userInfo.name);
				pstmt_upd->executeUpdate();
			}
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
		// 准备调用存储过程
		unique_ptr < sql::PreparedStatement > stmt(con->_con->prepareStatement("CALL test_procedure(?,@userId,@userName)"));
		// 设置输入参数
		stmt->setString(1, email);
		
		// 由于PreparedStatement不直接支持注册输出参数，我们需要使用会话变量或其他方法来获取输出参数的值

		  // 执行存储过程
		stmt->execute();
		// 如果存储过程设置了会话变量或有其他方式获取输出参数的值，你可以在这里执行SELECT查询来获取它们
	   // 例如，如果存储过程设置了一个会话变量@result来存储输出结果，可以这样获取：
		unique_ptr<sql::Statement> stmtResult(con->_con->createStatement());
		unique_ptr<sql::ResultSet> res(stmtResult->executeQuery("SELECT @userId AS uid"));
		if (!(res->next())) {
			return false;
		}
		
		uid = res->getInt("uid");
		cout << "uid: " << uid << endl;
		
		stmtResult.reset(con->_con->createStatement());
		res.reset(stmtResult->executeQuery("SELECT @userName AS name"));
		if (!(res->next())) {
			return false;
		}
		
		name = res->getString("name");
		cout << "name: " << name << endl;
		return true;

	}
	catch (sql::SQLException& e) {
		std::cerr << "SQLException: " << e.what();
		std::cerr << " (MySQL error code: " << e.getErrorCode();
		std::cerr << ", SQLState: " << e.getSQLState() << " )" << std::endl;
		return false;
	}
}
