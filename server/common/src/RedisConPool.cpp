#include "RedisConPool.h"

#include <chrono>
#include <exception>
#include <iostream>

RedisConPool::RedisConPool(std::size_t poolSize, std::string host, int port, std::string password)
	: b_stop_(false), poolSize_(poolSize), host_(std::move(host)), port_(port),
	pwd_(std::move(password)), fail_count_(0), counter_(0) {
	for (std::size_t i = 0; i < poolSize_; ++i) {
		auto* context = redisConnect(host_.c_str(), port_);
		if (context == nullptr || context->err != 0) {
			if (context != nullptr) {
				redisFree(context);
			}
			continue;
		}

		auto* reply = (redisReply*)redisCommand(context, "AUTH %s", pwd_.c_str());
		if (reply == nullptr || reply->type == REDIS_REPLY_ERROR) {
			std::cout << "redis auth failed" << std::endl;
			if (reply != nullptr) {
				freeReplyObject(reply);
			}
			redisFree(context);
			continue;
		}

		freeReplyObject(reply);
		std::cout << "redis auth success" << std::endl;
		connections_.push(context);
	}

	check_thread_ = std::thread([this]() {
		while (!b_stop_) {
			counter_++;
			if (counter_ >= 60) {
				checkThreadPro();
				counter_ = 0;
			}

			// 每秒醒一次检查停止标志；Close() 通过 stop_cv_ 立即唤醒
			std::unique_lock<std::mutex> stop_lock(stop_mutex_);
			stop_cv_.wait_for(stop_lock, std::chrono::seconds(1),
				[this] { return b_stop_.load(); });
		}
	});
}

RedisConPool::~RedisConPool() {
	// 与显式 Close() 同一幂等关闭路径，保证检测线程被回收
	Close();
}

void RedisConPool::ClearConnections() {
	std::lock_guard<std::mutex> lock(mutex_);
	while (!connections_.empty()) {
		auto* context = connections_.front();
		redisFree(context);
		connections_.pop();
	}
}

redisContext* RedisConPool::getConnection() {
	std::unique_lock<std::mutex> lock(mutex_);
	cond_.wait(lock, [this] {
		if (b_stop_) {
			return true;
		}
		return !connections_.empty();
		});
	// 池停止，直接返回空指针
	if (b_stop_) {
		return nullptr;
	}
	auto* context = connections_.front();
	connections_.pop();
	return context;
}

redisContext* RedisConPool::getConNonBlock() {
	std::unique_lock<std::mutex> lock(mutex_);
	if (b_stop_) {
		return nullptr;
	}

	if (connections_.empty()) {
		return nullptr;
	}

	auto* context = connections_.front();
	connections_.pop();
	return context;
}

void RedisConPool::returnConnection(redisContext* context) {
	if (context == nullptr) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!b_stop_) {
			connections_.push(context);
			cond_.notify_one();
			return;
		}
	}
	// 池已停止：归还的连接立即释放，避免泄漏
	redisFree(context);
}

void RedisConPool::Close() {
	// 幂等：仅首个调用者执行关闭流程
	if (b_stop_.exchange(true)) {
		return;
	}
	cond_.notify_all();
	stop_cv_.notify_all();
	if (check_thread_.joinable()) {
		check_thread_.join();
	}
	ClearConnections();
}

bool RedisConPool::reconnect() {
	auto* context = redisConnect(host_.c_str(), port_);
	if (context == nullptr || context->err != 0) {
		if (context != nullptr) {
			redisFree(context);
		}
		return false;
	}

	auto* reply = (redisReply*)redisCommand(context, "AUTH %s", pwd_.c_str());
	if (reply == nullptr || reply->type == REDIS_REPLY_ERROR) {
		std::cout << "redis auth failed" << std::endl;
		if (reply != nullptr) {
			freeReplyObject(reply);
		}
		redisFree(context);
		return false;
	}

	freeReplyObject(reply);
	std::cout << "redis auth success" << std::endl;
	returnConnection(context);
	return true;
}

void RedisConPool::checkThreadPro() {
	size_t pool_size;
	{
		// 读取当前队列长度
		std::lock_guard<std::mutex> lock(mutex_);
		pool_size = connections_.size();
	}

	for (int i = 0; i < static_cast<int>(pool_size) && !b_stop_; ++i) {
		// 1) 取出一个连接（非阻塞）
		auto* context = getConNonBlock();
		if (context == nullptr) {
			break;
		}

		redisReply* reply = nullptr;
		try {
			reply = (redisReply*)redisCommand(context, "PING");
			// 2. 先看底层 I/O/协议层有没有错
			if (context->err) {
				std::cout << "Connection error: " << context->err << std::endl;
				if (reply) {
					freeReplyObject(reply);
				}
				redisFree(context);
				fail_count_++;
				continue;
			}

			// 3. 再看 Redis 服务器回的是不是 ERROR
			if (!reply || reply->type == REDIS_REPLY_ERROR) {
				std::cout << "reply is null, redis ping failed: " << std::endl;
				if (reply) {
					freeReplyObject(reply);
				}
				redisFree(context);
				fail_count_++;
				continue;
			}
			// 4. 连接没有问题，则还回去
			freeReplyObject(reply);
			returnConnection(context);
		}
		catch (std::exception& exp) {
			if (reply) {
				freeReplyObject(reply);
			}

			redisFree(context);
			fail_count_++;
		}
	}

	// 执行重连操作
	while (fail_count_ > 0) {
		auto res = reconnect();
		if (res) {
			fail_count_--;
		}
		else {
			// 重连失败，下一周期再试
			break;
		}
	}
}
