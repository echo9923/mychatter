#include <csignal>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include <boost/asio.hpp>
#include <boost/filesystem.hpp>
#include "AsioIOServicePool.h"
#include "CServer.h"
#include "ConfigMgr.h"
#include "MysqlMgr.h"
#include "const.h"

namespace {
/// 本地时间格式化为 MySQL timestamp 字符串（与 chat_messages.created_at 同构）
std::string FormatDbTime(std::time_t t) {
	std::tm tm_val{};
	localtime_s(&tm_val, &t);
	char buf[32];
	std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_val);
	return buf;
}

/// 文件 mtime；不存在/出错返回 0
std::time_t FileMTime(const boost::filesystem::path& p) {
	boost::system::error_code ec;
	if (!boost::filesystem::exists(p, ec)) {
		return 0;
	}
	std::time_t t = boost::filesystem::last_write_time(p, ec);
	return ec ? 0 : t;
}

/**
 * @brief 资源清理任务（启动时 + 每小时）
 *
 * 1. DB：捞取 created_at 早于 7 天前且 status=PENDING 的资源消息，对应 .part
 *    缺失或 mtime 陈旧的标记为 FAILED。未发布资源不创建 user_events 行，
 *    因此不会出现在接收者的统一事件流中。
 * 2. 磁盘：删 mtime 超 7 天的 *.part；删 mtime 超 7 天且 DB 无行/非 Ready 的最终文件。
 *
 * 与活跃上传的竞争防护：活跃上传每片都刷新 .part 的 mtime，天然新鲜不会被清理；
 * DB 侧条件更新（status=PENDING）防二次标记。残余竞态后果 = 客户端收
 * ResourceStateInvalid 后本地标失败，可重发，无数据损坏。
 */
void RunResourceCleanup() {
	const std::time_t now = std::time(nullptr);
	const std::string before = FormatDbTime(now - kResourceRetentionSeconds);
	const auto root = ConfigMgr::Inst().GetResourceRootPath();

	//1) DB 侧：过期标记
	std::vector<ExpiredResource> pending;
	if (MysqlMgr::GetInstance()->GetExpiredResourceIds(before, 200, pending)) {
		std::vector<ExpiredResource> to_expire;
		for (const auto& item : pending) {
			auto part_path = root / std::to_string(item.sender_user_id)
				/ (std::to_string(item.message_id) + ".part");
			const std::time_t mtime = FileMTime(part_path);
			//.part 缺失或 7 天未动 → 终态过期
			if (mtime == 0 || mtime < now - kResourceRetentionSeconds) {
				to_expire.push_back(item);
			}
		}
		if (!to_expire.empty()) {
			if (MysqlMgr::GetInstance()->MarkResourceExpired(to_expire)) {
				std::cout << "ResourceCleanup: expired " << to_expire.size()
					<< " stale resource message(s)" << std::endl;
			}
			else {
				std::cerr << "ResourceCleanup: MarkResourceExpired failed" << std::endl;
			}
		}
	}

	//2) 磁盘侧：陈旧文件回收（只扫 resource/，与头像目录隔离）
	boost::system::error_code ec;
	if (!boost::filesystem::exists(root, ec)) {
		return;
	}
	for (boost::filesystem::recursive_directory_iterator it(root, ec), end;
		it != end && !ec; it.increment(ec)) {
		if (!boost::filesystem::is_regular_file(it->path(), ec)) {
			continue;
		}
		const std::time_t mtime = FileMTime(it->path());
		if (mtime == 0 || mtime >= now - kResourceRetentionSeconds) {
			continue; //新鲜文件不清理
		}
		const std::string name = it->path().filename().string();
		bool removable = false;
		if (name.size() > 5 && name.compare(name.size() - 5, 5, ".part") == 0) {
			removable = true; //陈旧 .part 直接回收（DB 侧已在上面的标记流程处理）
		}
		else {
			//最终文件：仅当 DB 无行或非 Ready 时回收（Ready 文件是接收方的下载源）
			char* parse_end = nullptr;
			const long long mid = std::strtoll(name.c_str(), &parse_end, 10);
			if (parse_end != nullptr && *parse_end == '\0' && mid > 0) {
				auto msg = MysqlMgr::GetInstance()->GetChatMsgById(mid);
				removable = !msg || msg->status != MessageStatus::Published;
			}
			else {
				removable = true; //不符合 <message_id> 命名规则的残留
			}
		}
		if (removable) {
			boost::system::error_code rm_ec;
			if (boost::filesystem::remove(it->path(), rm_ec)) {
				std::cout << "ResourceCleanup: removed stale file " << it->path().string() << std::endl;
			}
		}
	}
}

/// 启动即跑一次，此后每小时一次；ec 非空（io_context 停止）时终止链
void ScheduleCleanup(boost::asio::steady_timer& timer) {
	RunResourceCleanup();
	timer.expires_after(std::chrono::hours(1));
	timer.async_wait([&timer](const boost::system::error_code& ec) {
		if (!ec) {
			ScheduleCleanup(timer);
		}
		});
}
} // namespace

int main()
{
	auto& cfg = ConfigMgr::Inst();

	std::shared_ptr<AsioIOServicePool> pool;
	try {
		pool = std::make_shared<AsioIOServicePool>(std::thread::hardware_concurrency());

		boost::asio::io_context  io_context;
		boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
		auto port_str = cfg["SelfServer"]["Port"];

		//资源清理定时器：启动时 + 每小时（挂主 io_context，与 acceptor 同线程串行）
		boost::asio::steady_timer cleanup_timer(io_context);
		cleanup_timer.expires_after(std::chrono::seconds(0));
		cleanup_timer.async_wait([&cleanup_timer](const boost::system::error_code& ec) {
			if (!ec) {
				ScheduleCleanup(cleanup_timer);
			}
			});

		CServer s(io_context, atoi(port_str.c_str()), pool);
		signals.async_wait([&io_context, pool, &s](auto, auto) {
			s.Stop();
			pool->Drain();
			io_context.stop();
			pool->Stop();
			});
		io_context.run();
	}
	catch (std::exception& e) {
		if (pool) {
			pool->Stop();
		}
		std::cerr << "Exception: " << e.what() << std::endl;
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
