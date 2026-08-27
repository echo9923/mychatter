#include "MySqlPool.h"

MySqlPool::MySqlPool(const std::string& url, const std::string& user, const std::string& pass, const std::string& schema, int poolSize)
	: url_(url), user_(user), pass_(pass), schema_(schema), poolSize_(poolSize), b_stop_(false), _fail_count(0) {
	try {
		for (int i = 0; i < poolSize_; ++i) {
			sql::mysql::MySQL_Driver* driver = sql::mysql::get_mysql_driver_instance();
			auto*  con = driver->connect(url_, user_, pass_);
			con->setSchema(schema_);
			// 获取当前时间戳
			auto currentTime = std::chrono::system_clock::now().time_since_epoch();
			// 将时间戳转换为秒
			long long timestamp = std::chrono::duration_cast<std::chrono::seconds>(currentTime).count();
			pool_.push(std::make_unique<SqlConnection>(con, timestamp));
			std::cout << "mysql connection init success" << std::endl;
		}

		// 健康检测线程：首次 60 秒后、此后每 60 秒执行一次 SELECT 1 保活。
		// 不再 detach：Close()/析构负责 join。每秒醒一次检查停止标志，
		// Close() 通过 stop_cv_ 立即唤醒。
		_check_thread = std::thread([this]() {
			int count = 0;
			while (!b_stop_) {
				if (count >= 60) {
					count = 0;
					checkConnectionPro();
				}
				std::unique_lock<std::mutex> stop_lock(stop_mutex_);
				stop_cv_.wait_for(stop_lock, std::chrono::seconds(1),
					[this] { return b_stop_.load(); });
				count++;
			}
		});
	}
	catch (sql::SQLException& e) {
		// 处理异常：连接创建失败时检测线程可能尚未启动，Close()/析构仍然安全
		std::cout << "mysql pool init failed, error is " << e.what() << std::endl;
	}
}

MySqlPool::~MySqlPool() {
	// 与显式 Close() 同一幂等关闭路径，不抛异常
	try {
		Close();
	}
	catch (...) {
	}
	std::unique_lock<std::mutex> lock(mutex_);
	while (!pool_.empty()) {
		pool_.pop();
	}
}

void MySqlPool::checkConnectionPro() {
	// 1)先读取“目标处理数”
	size_t targetCount;
	{
		std::lock_guard<std::mutex> guard(mutex_);
		targetCount = pool_.size();
	}

	//2 当前已经处理的数量
	size_t processed = 0;

	//3 时间戳
	auto now = std::chrono::system_clock::now().time_since_epoch();
	long long timestamp = std::chrono::duration_cast<std::chrono::seconds>(now).count();

	while (processed < targetCount) {
		std::unique_ptr<SqlConnection> con;
		{
			std::lock_guard<std::mutex> guard(mutex_);
			if (pool_.empty()) {
				break;
			}
			con = std::move(pool_.front());
			pool_.pop();
		}

		bool healthy = true;
		//解锁后做检查/重连逻辑
		if (timestamp - con->_last_oper_time >= 5) {
			try {
				std::unique_ptr<sql::Statement> stmt(con->_con->createStatement());
				stmt->executeQuery("SELECT 1");
				con->_last_oper_time = timestamp;
			}
			catch (sql::SQLException& e) {
				std::cout << "Error keeping connection alive: " << e.what() << std::endl;
				healthy = false;
				_fail_count++;
			}

		}

		if (healthy)
		{
			std::lock_guard<std::mutex> guard(mutex_);
			pool_.push(std::move(con));
			cond_.notify_one();
		}

		++processed;
	}

	while (_fail_count > 0) {
		auto b_res = reconnect(timestamp);
		if (b_res) {
			_fail_count--;
		}
		else {
			break;
		}
	}
}

bool MySqlPool::reconnect(long long timestamp) {
	if (b_stop_) {
		return false;
	}
	try {

		sql::mysql::MySQL_Driver* driver = sql::mysql::get_mysql_driver_instance();
		auto* con = driver->connect(url_, user_, pass_);
		con->setSchema(schema_);

		auto newCon = std::make_unique<SqlConnection>(con, timestamp);
		{
			std::lock_guard<std::mutex> guard(mutex_);
			if (b_stop_) {
				return false;
			}
			pool_.push(std::move(newCon));
		}
		std::cout << "mysql connection reconnect success" << std::endl;
		return true;

	}
	catch (sql::SQLException& e) {
		std::cout << "Reconnect failed, error is " << e.what() << std::endl;
		return false;
	}
}

std::unique_ptr<SqlConnection> MySqlPool::getConnection() {
	std::unique_lock<std::mutex> lock(mutex_);
	cond_.wait(lock, [this] {
		if (b_stop_) {
			return true;
		}
		return !pool_.empty(); });
	if (b_stop_) {
		return nullptr;
	}
	std::unique_ptr<SqlConnection> con(std::move(pool_.front()));
	pool_.pop();
	return con;
}

void MySqlPool::returnConnection(std::unique_ptr<SqlConnection> con) {
	if (!con || b_stop_) {
		return;
	}
	try {
		// Every borrower receives a clean autocommit connection. Transactional DAO
		// methods commit explicitly; rollback here only clears a read snapshot or a
		// failed transaction that a caller left open.
		if (!con->_con->getAutoCommit()) {
			con->_con->rollback();
			con->_con->setAutoCommit(true);
		}
		const auto now = std::chrono::system_clock::now().time_since_epoch();
		con->_last_oper_time = std::chrono::duration_cast<std::chrono::seconds>(now).count();
	}
	catch (const sql::SQLException& e) {
		std::cout << "Error resetting returned MySQL connection: " << e.what() << std::endl;
		const auto now = std::chrono::system_clock::now().time_since_epoch();
		const auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(now).count();
		if (!reconnect(timestamp)) {
			_fail_count++;
		}
		return;
	}
	std::unique_lock<std::mutex> lock(mutex_);
	if (b_stop_) {
		return;
	}
	pool_.push(std::move(con));
	cond_.notify_one();
}

void MySqlPool::Close() {
	// 幂等：仅首个调用者执行关闭流程
	if (b_stop_.exchange(true)) {
		return;
	}
	cond_.notify_all();
	stop_cv_.notify_all();
	if (_check_thread.joinable()) {
		_check_thread.join();
	}
}
