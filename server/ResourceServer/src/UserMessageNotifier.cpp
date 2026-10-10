#include "UserMessageNotifier.h"
#include "ChatServerGrpcClient.h"
#include "RedisMgr.h"
#include "const.h"

#include <string>

namespace {

/// 对端 ChatServer RECIPIENT_OFFLINE 应用层错误码（20xx 通用表；只记录不重试，
/// 增量同步兜底）
constexpr int kAppRecipientOffline = llfc_proto::ERR_RECIPIENT_OFFLINE;

} // namespace

UserMessageNotifier::UserMessageNotifier()
{
	_thread = std::thread(&UserMessageNotifier::RunLoop, this);
}

UserMessageNotifier::~UserMessageNotifier()
{
	Stop();
}

bool UserMessageNotifier::Post(long long message_id, int event_type,
	int recipient_user_id) {
	if (_stopping.load()) {
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(_mutex);
		if (_stopping.load()) {
			return false;
		}
		if (_queue.size() >= kNotifierQueueLimit) {
			//有界队列满：丢弃并记日志。消息和 user_events 已提交，
			//接收者由 1405/1406 增量同步兜底。
			std::cerr << "UserMessageNotifier: queue full, drop notify msg="
				<< message_id << " (incremental sync will deliver)" << std::endl;
			return false;
		}
		_queue.push_back(NotifyJob{ message_id, event_type, recipient_user_id });
	}
	_cv.notify_one();
	return true;
}

void UserMessageNotifier::Stop() {
	if (_stopping.exchange(true)) {
		return;
	}
	{
		//唤醒工作线程；队列中剩余任务直接放弃（见类注释）
		std::lock_guard<std::mutex> lock(_mutex);
		_queue.clear();
	}
	_cv.notify_all();
	if (_thread.joinable()) {
		_thread.join();
	}
}

void UserMessageNotifier::RunLoop() {
	while (true) {
		NotifyJob job{};
		{
			std::unique_lock<std::mutex> lock(_mutex);
			_cv.wait(lock, [this]() {
				return _stopping.load() || !_queue.empty();
			});
			if (_stopping.load()) {
				return;
			}
			job = _queue.front();
			_queue.pop_front();
		}

		//Redis 在线查询与带退避的 gRPC 都只在本线程执行：
		//单线程使任务按入队顺序串行执行且绝不阻塞 FileWorker；跨 worker
		//的发布提交与入队可能重排，接收端 1405 缺口同步兜底。
		const auto uid_ip_key = USERIPPREFIX + std::to_string(job.recipient_user_id);
		std::string uid_ip_value;
		if (!RedisMgr::GetInstance()->Get(uid_ip_key, uid_ip_value)) {
			//接收者未登录：user_events 已提交，由增量同步兜底
			continue;
		}

		const auto notify = ChatServerGrpcClient::GetInstance()->NotifyUserMessage(
			job.message_id, job.event_type, job.recipient_user_id, uid_ip_value);
		if (notify.app_error == kAppRecipientOffline) {
			std::cout << "UserMessageNotifier: recipient offline msg="
				<< job.message_id << ", event_seq committed for incremental sync"
				<< std::endl;
		}
		else if (notify.app_error != ErrorCodes::Success) {
			std::cerr << "UserMessageNotifier: NotifyUserMessage failed msg="
				<< job.message_id << " grpc_code=" << notify.grpc_code
				<< " app_error=" << notify.app_error
				<< " (incremental sync will deliver)" << std::endl;
		}
	}
}
