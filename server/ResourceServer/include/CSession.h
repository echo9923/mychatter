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
private:
	bool BeginDrain();
	void asyncReadFull(std::size_t maxLength, std::function<void(const boost::system::error_code& , std::size_t)> handler);
	void asyncReadLen(std::size_t  read_len, std::size_t total_len,
		std::function<void(const boost::system::error_code&, std::size_t)> handler);
	
	
	void HandleWrite(const boost::system::error_code& error, std::shared_ptr<CSession> shared_self);
	//并发约束：所有 _socket 成员调用只能在所属 IO 线程执行（跨线程入口 Start/Send/Close 一律 post）
	tcp::socket _socket;
	//SendAndClose 的有界排空定时器，防止对端不读导致 Draining 永久悬挂
	boost::asio::steady_timer _drain_timer;
	std::string _session_id;
	char _data[MAX_LENGTH];
	CServer* _server;
	llfc::SessionLifecycle _lifecycle;
	std::queue<shared_ptr<SendNode> > _send_que;
	std::mutex _send_lock;
	std::mutex _lifecycle_mtx;
	//收到的消息结构
	std::shared_ptr<RecvNode> _recv_msg_node;
	bool _b_head_parse;
	//收到的头部结构
	std::shared_ptr<MsgNode> _recv_head_node;
	std::atomic<bool> _authed{false};
	std::atomic<int> _user_uid{0};
};


