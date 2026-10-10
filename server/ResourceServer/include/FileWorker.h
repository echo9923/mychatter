#pragma once
#include <thread>
#include <mutex>
#include <fstream>
#include <queue>
#include <condition_variable>
#include <nlohmann/json.hpp>
using json = nlohmann::json;
#include <functional>
#include <chrono>
#include <unordered_map>
#include "const.h"
#include "Sha256.h"
#include "LogicWorker.h"  // server/common 执行器
class CSession;
struct ChatMessage;
class SessionTaskQuota;

// ---------- 头像通道任务（1601/1603 旧协议，保持不变） ----------
struct FileTask {
	FileTask(std::shared_ptr<CSession> session,  MSG_TYPES msg_type, int uid, std::string path, std::string name,
		int seq, int total_size, int trans_size, int last,
		std::string file_data,
		std::function<void(const json&)> callback,long long chat_msg_id=0,
		int sender = 0, int receiver = 0,
		std::shared_ptr<SessionTaskQuota> quota = nullptr) :_session(session), _msg_type(msg_type),_uid(uid),
		_seq(seq), _path(path), _name(name), _total_size(total_size),
		_trans_size(trans_size), _last(last), _file_data(file_data), _callback(callback), _chat_msg_id(chat_msg_id),
		_sender(sender), _receiver(receiver), _quota(std::move(quota))
	{}
	~FileTask(){}
	std::shared_ptr<CSession> _session;
	MSG_TYPES _msg_type;
	int _uid;
	int _seq ;
	std::string _path;
	std::string _name ;
	int _total_size ;
	int _trans_size ;
	int _last ;
	std::string _file_data;
	std::function<void(const json&)>  _callback;  //添加回调函数
	long long _chat_msg_id;   // 64 位：关联 chat_messages.message_id
	int _sender;
	int _receiver;
	long long _thread_id;     // 64 位
	/// 会话在途字节令牌：任务销毁即归还，见 CSession::SessionTaskQuota
	std::shared_ptr<SessionTaskQuota> _quota;
};


struct DownloadTask {
	DownloadTask(std::shared_ptr<CSession> session, int uid, std::string name,
		int seq, std::string file_path,
		std::function<void(const json&)> callback,
		std::shared_ptr<SessionTaskQuota> quota = nullptr) :_session(session), _uid(uid),
		_seq(seq), _name(name), _file_path(file_path), _callback(callback),
		_quota(std::move(quota))
	{}
	~DownloadTask() {}
	std::shared_ptr<CSession> _session;
	int _uid;
	int _seq;
	std::string _name;
	std::string _file_path;
	std::function<void(const json&)>  _callback;  //添加回调函数
	/// 会话在途字节令牌（首层 1603 传递而来）：任务销毁即归还（RAII）
	std::shared_ptr<SessionTaskQuota> _quota;
};

/// 上传一个分片（1505）：data 为 Base64，offset 为该分片在整文件中的字节偏移
struct ResourceChunkTask {
	ResourceChunkTask(std::shared_ptr<CSession> session, long long message_id,
		long long offset, std::string chunk_sha256, std::string file_data,
		std::function<void(const json&)> callback,
		std::shared_ptr<SessionTaskQuota> quota = nullptr)
		:_session(session), _message_id(message_id), _offset(offset),
		_chunk_sha256(std::move(chunk_sha256)), _file_data(std::move(file_data)),
		_callback(std::move(callback)), _quota(std::move(quota))
	{}
	std::shared_ptr<CSession> _session;
	long long _message_id;
	long long _offset;
	std::string _chunk_sha256;  ///< 分片内容 SHA-256（小写 hex）
	std::string _file_data;     ///< Base64 编码的分片数据
	std::function<void(const json&)> _callback;
	/// 会话在途字节令牌：任务销毁即归还（RAII）
	std::shared_ptr<SessionTaskQuota> _quota;
};

/// 查询资源下载信息（1509）：与 1511 一样固定路由到 DownloadWorker[mid%N]，
/// 由同一线程完成鉴权/DB 读取/回包，天然有序。
struct ResourceDownInfoTask {
	ResourceDownInfoTask(std::shared_ptr<CSession> session, long long message_id,
		std::function<void(const json&)> callback,
		std::shared_ptr<SessionTaskQuota> quota = nullptr)
		:_session(std::move(session)), _message_id(message_id),
		_callback(std::move(callback)), _quota(std::move(quota))
	{}
	std::shared_ptr<CSession> _session;
	long long _message_id;
	std::function<void(const json&)> _callback;
	/// 会话在途字节令牌（首层传递而来）：任务销毁即归还
	std::shared_ptr<SessionTaskQuota> _quota;
};

/// 下载一个分片（1511）：从最终文件（已通过 .part 原子改名）按 offset 读取。
/// 文件路径/大小由 DownloadWorker 内的已发布资源缓存解析（不再逐片查 DB）。
struct ResourceChunkDownTask {
	//P1c：1512 二进制回包——meta JSON + raw 分片一并回调
	ResourceChunkDownTask(std::shared_ptr<CSession> session, long long message_id,
		long long offset,
		std::function<void(const json&, const std::string&)> callback,
		std::shared_ptr<SessionTaskQuota> quota = nullptr)
		:_session(std::move(session)), _message_id(message_id), _offset(offset),
		_callback(std::move(callback)), _quota(std::move(quota))
	{}
	std::shared_ptr<CSession> _session;
	long long _message_id;
	long long _offset;
	std::function<void(const json&, const std::string&)> _callback;
	/// 会话在途字节令牌（首层传递而来）：任务销毁即归还
	std::shared_ptr<SessionTaskQuota> _quota;
};

/// FileWorker 内存中的上传会话：仅固定 worker 线程访问，无锁。
/// 重启/逐出后按 MySQL 和 .part 重建，先回退末尾一片，再恢复已确认进度。
struct UploadSession {
	long long message_id = 0;
	long long file_size_bytes = 0;
	std::string sha256;  ///< 整文件 SHA-256（来自 message_resources.sha256）
	unsigned long long received = 0;  ///< 已确认字节数；失败写入的尾巴不计入进度
	int sender_user_id = 0;          ///< 会话持有者（权限校验：上传者必须是消息 sender）
	int recipient_user_id = 0;
	std::chrono::steady_clock::time_point last_active;
	/// 同一会话整文件哈希不符累计次数；达到 3 次标记 Failed 并回 2116
	int whole_hash_mismatches = 0;

	// 流式哈希（P1a-2）：hash_valid 为真当且仅当 hasher 恰好覆盖 [0, received)
	// 且未 finalize。只有从 0 连续追加才成立；会话重建 received>0、重复片
	// 校验失败截断、任何写失败都会置为无效，完成点回退 Sha256FileHex 整文件
	// 重算。final_hex 缓存完成点结果，重试完成不重复 finalize。
	llfc::Sha256Stream hasher;  ///< 只可移动 => UploadSession 亦只可移动
	bool hash_valid = false;
	std::string final_hex;

	//P2 秒传挑战（绑定本会话 + 确切请求连接，单次使用 + 有限时效）：
	//1507 命中"发送者 Published 历史 + blob 存在 + 大小一致"时生成；
	//1513 消费（证明不符同时抑制本会话再次下发，破无限挑战循环）、
	//TTL 过期、会话逐出/服务重启即失效。客户端按 2118 回退普通上传
	//并重发 1507 重新获取（新会话不受旧抑制影响，但客户端 2118 后
	//本传输内禁用 proof，闭环兜底）。
	std::string challenge_nonce_raw;       ///< 16 字节原始 nonce（下发 hex）
	unsigned long long challenge_offset = 0;
	unsigned long long challenge_length = 0;
	bool challenge_active = false;
	/// 挑战只认下发时那条 CSession：其他连接（即便同 uid 重连）重放 1513 无效
	std::weak_ptr<CSession> challenge_session;
	std::chrono::steady_clock::time_point challenge_issued_at; ///< TTL 起点
	/// 证明不符后置真：本会话不再下发新挑战（无限挑战循环的服务端闸门）
	bool challenge_suppressed = false;
};

class FileWorker
{
public:
	FileWorker();
	~FileWorker();
	void RegisterHandlers();
	/// 幂等停机：拒绝新任务并排空既有任务（透传 common 执行器语义）
	void Stop();
	/// 头像通道（1601）：按调用方给定的 index 投递
	void PostTask(std::shared_ptr<FileTask> task);
	/// 资源分片上传（1505）：必须在 ResourceWorkerIndex 选定的 worker 上执行
	void PostChunkTask(std::shared_ptr<ResourceChunkTask> task);
	/// 任意闭包（1507 进度查询等需访问 _upload_sessions 的操作，同 worker 串行化）
	void PostClosure(std::function<void(FileWorker&)> fn);

	/// 上传会话缓存（仅本 worker 线程内调用）：未命中返回 nullptr
	std::shared_ptr<UploadSession> FindUploadSession(long long message_id);
	/// 从 MySQL 行 + .part 重建会话并回退末尾一片（仅本 worker 线程内调用）
	std::shared_ptr<UploadSession> LoadUploadSession(long long message_id,
		const std::shared_ptr<ChatMessage>& msg);
	/// 逐出上传会话缓存（仅本 worker 线程内调用；1507 复核到终态时同步清理）
	void EvictUploadSession(long long message_id);

	//P2 秒传证明（1513，仅本 worker 线程）：DB 真值复核（uid 必须为本消息
	//sender；已 Published 直接回成功携带原 event_seq，不产生新事件）→
	//会话挑战（一次性、TTL、绑定下发时的 req_session）→ 常量时间校验 →
	//与普通上传共用发布事务与 Notifier → 回 1514（证明不符/挑战丢失回
	//2118，客户端回退普通上传）。
	void HandleInstantProof(std::shared_ptr<CSession> req_session, int req_uid,
		long long message_id, const std::string& proof_hex,
		const std::function<void(const json&)>& respond);

private:
	void task_callback(std::shared_ptr<FileTask>);
	//资源上传分片状态机（1505）：校验-写入-完成判定
	void HandleResourceChunk(std::shared_ptr<ResourceChunkTask> task);
	//完成点：整文件 SHA-256 校验（3 次不符转 Failed）-> flush -> 持久化 rename
	//-> 发布事务 -> 回 1506 -> 投递 UserMessageNotifier（gRPC 不在本线程）
	void CompleteResourceUpload(std::shared_ptr<ResourceChunkTask> task,
		std::shared_ptr<UploadSession> session);
	//空闲会话逐出（30 分钟无活动）
	void EvictIdleUploadSessions();

	std::unordered_map<MSG_TYPES, std::function<void(std::shared_ptr<FileTask>)> > _handlers;
	///< 上传会话表：key=message_id，仅 worker 线程访问（固定路由保证单线程访问）
	std::unordered_map<long long, std::shared_ptr<UploadSession>> _upload_sessions;
	/// 执行器（common ::LogicWorker）。必须声明为最后一个成员：其析构会排空
	/// 队列并执行仍引用本对象其余成员的任务，届时其余成员必须仍存活。
	::LogicWorker _executor;
};


class DownloadWorker {
public:
	DownloadWorker();
	~DownloadWorker();
	/// 幂等停机：拒绝新任务并排空既有任务（透传 common 执行器语义）
	void Stop();
	void PostTask(std::shared_ptr<DownloadTask> task);
	/// 1509 下载信息查询：必须在 ResourceWorkerIndex 选定的 worker 上执行
	void PostDownInfoTask(std::shared_ptr<ResourceDownInfoTask> task);
	/// 资源分片下载（1511）：必须在 ResourceWorkerIndex 选定的 worker 上执行
	void PostChunkTask(std::shared_ptr<ResourceChunkDownTask> task);
private:
	void task_callback(std::shared_ptr<DownloadTask>);
	void HandleResourceDownInfo(std::shared_ptr<ResourceDownInfoTask> task);
	void HandleResourceChunkDown(std::shared_ptr<ResourceChunkDownTask> task);

	/// 已发布资源的鉴权/元数据缓存（仅本 worker 线程访问）。
	/// Published 行的资源字段不可变，缓存后对任何会话可直接比对 uid；
	/// first_session 记录首次鉴权会话（weak，不延长生命周期）。
	struct PublishedCacheEntry {
		std::weak_ptr<CSession> first_session;
		int sender_user_id = 0;
		int recipient_user_id = 0;
		int message_type = 0;
		std::string file_path;
		unsigned long long file_size_bytes = 0;
		std::string sha256;
		std::string original_file_name;
		std::string mime_type;
	};
	/// 鉴权缓存条目上限；超限整体清空（下轮回查 DB，实现保持无趣）
	static constexpr std::size_t kPublishedCacheLimit = 1024;

	/// 打开的最终文件句柄（仅本 worker 线程访问）：LRU 上限 64，空闲 60s 淘汰
	struct FileHandleEntry {
		std::ifstream in;
		std::chrono::steady_clock::time_point last_used;
	};
	static constexpr std::size_t kFileHandleLimit = 64;
	static constexpr auto kFileHandleIdle = std::chrono::seconds(60);

	/// 解析 mid 的已发布元数据：缓存命中直接返回（Published 行字段不可变，
	/// 按缓存行比对 uid）；未命中回查 DB（权限通过且 Published 才入缓存）。
	enum class ResolveStatus { Authorized, Forbidden, NotReady, NotFound, DbError };
	ResolveStatus ResolvePublished(const std::shared_ptr<CSession>& session,
		long long message_id, int uid, const PublishedCacheEntry** out_entry);
	void EvictIdleFileHandles();

	std::unordered_map<long long, PublishedCacheEntry> _published_cache;
	std::unordered_map<long long, FileHandleEntry> _file_handles;
	/// 执行器（common ::LogicWorker）。必须声明为最后一个成员：其析构会排空
	/// 队列并执行仍引用本对象其余成员的任务，届时其余成员必须仍存活。
	::LogicWorker _executor;
};
