#pragma once
#include <boost/asio.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast.hpp>
#include <boost/asio.hpp>
#include <atomic>
#include <queue>
#include <mutex>
#include <memory>
#include <string>
#include "const.h"
#include "MsgNode.h"
#include "SessionLifecycle.h"
using namespace std;


namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace net = boost::asio;            // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>


class CServer;
class LogicSystem;

class CSession: public std::enable_shared_from_this<CSession>
{
public:
	CSession(boost::asio::io_context& io_context, CServer* server);
	~CSession();
	tcp::socket& GetSocket();
	std::string& GetSessionId();
	bool TrySetAuth(int uid);
	bool IsAuthed() const;
	int GetUserId() const;
	bool IsOpen() const noexcept;
	void Start();
	void Send(std::string msg, short msg_type);
	void SendAndClose(std::string msg, short msg_type);
	void Close();
	std::shared_ptr<CSession> SharedSelf();
	void AsyncReadBody(int length);
	void AsyncReadHead(int total_len);

	/// 会话在途任务字节数配额（RAII 令牌随任务传递）：任务销毁即归还。
	/// 占用字节数计入 _inflight_task_bytes，超过 kMaxSessionInflightTaskBytes
	/// 拒绝（调用方回 2016），防单连接用大分片任务灌满 worker 队列。
	bool TryAcquireTaskBytes(std::size_t bytes);
	void ReleaseTaskBytes(std::size_t bytes);
private:
	bool BeginDrain();
	//直接读入调用方缓冲（包头为固定 _head_data，包体为 RecvNode 精确分配）
	void asyncReadFull(char* dest, std::size_t total_len,
		std::function<void(const boost::system::error_code&, std::size_t)> handler);
	void asyncReadLen(char* dest, std::size_t read_len, std::size_t total_len,
		std::function<void(const boost::system::error_code&, std::size_t)> handler);

	void HandleWrite(const boost::system::error_code& error, std::shared_ptr<CSession> shared_self);

	/// 清空 _send_que 并对每帧按 SendNode::_total_len 对称归还发送预算
	/// （写失败/异常路径）。调用方必须已持有 _send_lock：此处只能内联
	/// 扣减 _send_pending_*，不得调用 ReleaseSendBudget（它会再次加锁）。
	void ClearSendQueueLocked();

	/// 发送预算：在 post 之前预留（跨线程调用点），同时覆盖“已 post 到 IO
	/// 线程但尚未入队”的闭包与已在 _send_que 中的帧；帧写入完成（HandleWrite
	/// pop）或未入队路径（RAII 归还）时释放。
	bool TryReserveSendBudget(std::size_t frame_bytes);
	void ReleaseSendBudget(std::size_t frame_bytes);
	/// 发送预留的 RAII 归还守卫：成功入队后 disarm，所有权移交队列。
	/// 持有 session 的 shared_ptr：guard 可能在会话其余引用先行释放后析构
	/// （如 io_context 停机销毁已 post 未执行的闭包），持有所有权保证
	/// ReleaseSendBudget 仍作用于存活对象。
	struct SendBudgetGuard {
		std::shared_ptr<CSession> session;
		std::size_t bytes;
		bool disarmed = false;
		SendBudgetGuard(std::shared_ptr<CSession> s, std::size_t b)
			: session(std::move(s)), bytes(b) {}
		/// 成功入队后调用：预算所有权移交 _send_que，guard 析构不再归还
		void disarm() { disarmed = true; }
		~SendBudgetGuard() {
			if (session && !disarmed) {
				session->ReleaseSendBudget(bytes);
			}
		}
		SendBudgetGuard(const SendBudgetGuard&) = delete;
		SendBudgetGuard& operator=(const SendBudgetGuard&) = delete;
	};
	//并发约束：所有 _socket 成员调用只能在所属 IO 线程执行（跨线程入口 Start/Send/Close 一律 post）
	tcp::socket _socket;
	//SendAndClose 的有界排空定时器，防止对端不读导致 Draining 永久悬挂
	boost::asio::steady_timer _drain_timer;
	std::string _session_id;
	//固定 6 字节包头缓冲；包体按帧大小分配 RecvNode，不再整帧 MAX_LENGTH 缓冲
	char _head_data[HEAD_TOTAL_LEN];
	CServer* _server;
	llfc::SessionLifecycle _lifecycle;
	std::queue<shared_ptr<SendNode> > _send_que;
	//发送预算（帧数/字节，含 6B 头）：见 TryReserveSendBudget；由 _send_lock 保护
	std::size_t _send_pending_frames = 0;
	std::size_t _send_pending_bytes = 0;
	std::mutex _send_lock;
	std::mutex _lifecycle_mtx;
	//收到的消息结构
	std::shared_ptr<RecvNode> _recv_msg_node;
	bool _b_head_parse;
	std::atomic<bool> _authed{false};
	std::atomic<int> _user_uid{0};
	//在途任务字节配额（RAII 令牌见 SessionTaskQuota）
	std::atomic<std::size_t> _inflight_task_bytes{0};
};

/// 会话在途任务字节令牌（RAII）：构造成功即占用配额，析构归还；
/// 持有 session 的 shared_ptr，保证归还时会话仍存活。
/// 任务结构以 shared_ptr<SessionTaskQuota> 持有，随任务在 worker 队列间传递。
class SessionTaskQuota
{
public:
	/// 尝试为 session 占用 bytes 配额；session 为空或超上限返回 nullptr
	static std::shared_ptr<SessionTaskQuota> Acquire(std::shared_ptr<CSession> session,
		std::size_t bytes);
	~SessionTaskQuota();
	SessionTaskQuota(const SessionTaskQuota&) = delete;
	SessionTaskQuota& operator=(const SessionTaskQuota&) = delete;
private:
	SessionTaskQuota(std::shared_ptr<CSession> session, std::size_t bytes);
	std::shared_ptr<CSession> _session;
	std::size_t _bytes;
};


