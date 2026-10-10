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
#include "FileSystem.h"
#include "LogicSystem.h"
#include "UserMessageNotifier.h"
#include "ResourceUploadFile.h" //llfc::ResourceUploadingPartPath / BlobInstallGcMutex（P2 CAS/GC）
#include <condition_variable>
#include <mutex>

namespace {
/// 未完成资源的保留时长（秒）：Pending 行 / .part / 未发布最终文件超过
/// 该时长即被清理任务标记 Failed 或删除
constexpr long long kResourceRetentionSeconds = 7LL * 24 * 3600;
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
 * 2. 磁盘（P2 内容寻址布局，只回收新 CAS 范围）：
 *    a) uploading/*.part：mtime 超 7 天直接回收（活跃上传每片刷新 mtime，
 *       天然新鲜不会被误删）；
 *    b) blobs/<xx>/<sha256>：mtime 超 7 天且无任何 Published 消息引用该
 *       sha256（引用判定只按内容身份，不附加磁盘 stat 大小；走
 *       idx_message_resources_sha）才回收。孤儿判定
 *       查询与 unlink 持 BlobInstallGcMutex，与发布侧"no-replace rename
 *       → DB 发布事务提交"临界区互斥，绝不会删掉正在发布/刚安装未提交
 *       的 blob；Transient（查库失败）一律保留待下轮。
 *    c) 旧 resource/<sender_uid>/<message_id> 布局残留：清理任务一律
 *       不动（项目约定不迁移旧数据，由部署流程统一清空重建，见
 *       开发文档/P2-秒传与内容寻址存储.md）。
 *    Windows 下文件被占用删除失败时保留，留到下一轮重试。
 *
 * 与活跃上传的竞争防护：活跃上传每片都刷新 .part 的 mtime，天然新鲜不会被清理；
 * DB 侧条件更新（status=PENDING）防二次标记。残余竞态后果 = 客户端收
 * ResourceStateInvalid 后本地标失败，可重发，无数据损坏。
 */
void RunResourceCleanup() {
	const std::time_t now = std::time(nullptr);
	const std::string before = FormatDbTime(now - kResourceRetentionSeconds);
	const auto root = ConfigMgr::Inst().GetResourceRootPath();

	//1) DB 侧：过期标记。(created_at, message_id) 游标分页扫完一轮——
	//旧行为 LIMIT 200 每轮固定取同批头部（.part 新鲜但 created_at 老的行
	//永远占住页头），页外的行被饿死；游标推进保证每行都有机会被检查。
	constexpr int kCleanupPageSize = 200;
	std::string cursor_created_at;
	long long cursor_message_id = 0;
	int expired_total = 0;
	while (true) {
		std::vector<ExpiredResource> page;
		if (!MysqlMgr::GetInstance()->GetExpiredResourceIds(before, cursor_created_at,
			cursor_message_id, kCleanupPageSize, page)) {
			//DB 异常：本轮放弃，下一小时从游标 0 重新扫
			break;
		}
		if (page.empty()) {
			break;
		}
		std::vector<ExpiredResource> to_expire;
		for (const auto& item : page) {
			cursor_created_at = item.created_at;
			cursor_message_id = item.message_id;
			//P2：进行中上传统一在 uploading/ 下
			const auto part_path = llfc::ResourceUploadingPartPath(root,
				item.message_id);
			const std::time_t mtime = FileMTime(part_path);
			//.part 缺失或 7 天未动 → 终态过期
			if (mtime == 0 || mtime < now - kResourceRetentionSeconds) {
				to_expire.push_back(item);
			}
		}
		if (!to_expire.empty()) {
			if (MysqlMgr::GetInstance()->MarkResourceExpired(to_expire)) {
				expired_total += static_cast<int>(to_expire.size());
			}
			else {
				std::cerr << "ResourceCleanup: MarkResourceExpired failed" << std::endl;
				break;
			}
		}
		if (page.size() < static_cast<std::size_t>(kCleanupPageSize)) {
			break;  //短页 = 本轮扫完
		}
	}
	if (expired_total > 0) {
		std::cout << "ResourceCleanup: expired " << expired_total
			<< " stale resource message(s)" << std::endl;
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
		//范围门禁：只回收新 CAS 布局（uploading/ 与 blobs/<xx>/）；
		//旧 resource/<sender_uid>/<message_id> 布局及其他残留一律不动
		//（项目约定不迁移旧数据，部署流程统一清空重建）。
		const std::string parent_dir = it->path().parent_path().filename().string();
		const bool in_uploading = parent_dir == "uploading";
		const bool in_blobs = parent_dir.size() == 2
			&& it->path().parent_path().parent_path().filename().string() == "blobs";
		if (!in_uploading && !in_blobs) {
			continue;
		}
		const std::time_t mtime = FileMTime(it->path());
		if (mtime == 0 || mtime >= now - kResourceRetentionSeconds) {
			continue; //新鲜文件不清理（含正在发布的 blob：rename 保留新写 mtime）
		}
		const std::string name = it->path().filename().string();
		if (in_uploading) {
			//uploading/ 只应有 <message_id>.part；陈旧即回收（DB 侧已在
			//上面的标记流程处理），被占用（Windows 句柄未释放等）保留下轮
			if (name.size() <= 5 || name.compare(name.size() - 5, 5, ".part") != 0) {
				continue; //非 .part 残留：交部署清理，不动
			}
			boost::system::error_code rm_ec;
			if (boost::filesystem::remove(it->path(), rm_ec)) {
				std::cout << "ResourceCleanup: removed stale part "
					<< it->path().string() << std::endl;
			}
			else if (rm_ec) {
				std::cerr << "ResourceCleanup: part remove deferred (in use?) "
					<< it->path().string() << " ec=" << rm_ec.message() << std::endl;
			}
			continue;
		}
		//blobs/<xx>/<sha256>：内容寻址孤儿判定（无 Published 引用才回收）。
		//查询 + unlink 持 BlobInstallGcMutex，与发布侧"安装 → DB 提交"
		//临界区互斥：不会删掉已安装但发布未提交的 blob。
		if (name.size() != 64) {
			continue; //不符合 sha256 命名的残留交部署清理，不动
		}
		//引用判定只按 sha256 身份：磁盘上的实际长度不参与查询——blob 的
		//内容身份由文件名（完整 SHA-256）确定，用 stat 大小做附加条件会
		//把"大小探测失败/被并发改写"的现有引用误判成孤儿并删除。
		std::lock_guard<std::mutex> cas_gc_lock(llfc::BlobInstallGcMutex());
		const auto ref = MysqlMgr::GetInstance()->BlobHasPublishedReference(name);
		if (ref != MysqlDao::TriState::No) {
			//Yes=仍被 Published 引用；Transient=查库失败——都保留待下轮
			continue;
		}
		boost::system::error_code rm_ec;
		if (boost::filesystem::remove(it->path(), rm_ec)) {
			std::cout << "ResourceCleanup: removed orphan blob "
				<< it->path().string() << std::endl;
		}
		else if (rm_ec) {
			//Windows 文件占用等：保留，下一轮重试
			std::cerr << "ResourceCleanup: blob remove deferred (in use?) "
				<< it->path().string() << " ec=" << rm_ec.message() << std::endl;
		}
	}
}

/// 清理调度线程：RunResourceCleanup（DB 分页查询 + 全树磁盘扫描）不再
/// 挂在主 io_context 上（原来会阻塞 acceptor 所在线程整轮扫描）。
/// 启动即跑一次，此后每小时一次；Stop 可中断休眠并 join。
class CleanupScheduler {
public:
	CleanupScheduler() = default;
	~CleanupScheduler() {
		Stop();
	}
	CleanupScheduler(const CleanupScheduler&) = delete;
	CleanupScheduler& operator=(const CleanupScheduler&) = delete;

	void Start() {
		_thread = std::thread(&CleanupScheduler::Run, this);
	}

	void Stop() {
		{
			std::lock_guard<std::mutex> lock(_mutex);
			_stop = true;
		}
		_cv.notify_all();
		if (_thread.joinable()) {
			_thread.join();
		}
	}

private:
	void Run() {
		while (true) {
			RunResourceCleanup();
			std::unique_lock<std::mutex> lock(_mutex);
			_cv.wait_for(lock, std::chrono::hours(1), [this]() { return _stop; });
			if (_stop) {
				return;
			}
		}
	}

	std::thread _thread;
	std::mutex _mutex;
	std::condition_variable _cv;
	bool _stop = false;
};
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

		//资源清理：独立线程（启动时 + 每小时），不占用 IO 线程
		CleanupScheduler cleanup;
		cleanup.Start();

		CServer s(io_context, atoi(port_str.c_str()), pool);
		//优雅停机顺序：停清理线程 → 停 accept/关闭会话 → 停 io →
		//排空入口 logic workers（排空期间仍可向 FileWorker 投递）→
		//排空 FileWorker/DownloadWorker → 显式停 Notifier（单例析构前）→
		//停 IO 池。各 Stop 幂等，静态析构重复调用安全。
		signals.async_wait([&io_context, pool, &s, &cleanup](auto, auto) {
			cleanup.Stop();
			s.Stop();
			io_context.stop();
			LogicSystem::GetInstance()->Stop();
			FileSystem::GetInstance()->Stop();
			UserMessageNotifier::GetInstance()->Stop();
			pool->Drain();
			pool->Stop();
		});
		io_context.run();
		cleanup.Stop();
		LogicSystem::GetInstance()->Stop();
		FileSystem::GetInstance()->Stop();
		UserMessageNotifier::GetInstance()->Stop();
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
