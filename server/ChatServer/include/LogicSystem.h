#pragma once
#include "Singleton.h"
#include "LogicWorker.h"
#include <atomic>
#include <memory>
#include <vector>
#include <queue>
#include <thread>
#include "CSession.h"
#include <queue>
#include <map>
#include <functional>
#include "const.h"
#include <nlohmann/json.hpp>
using json = nlohmann::json;
#include <unordered_map>
#include "data.h"

class CServer;
/// 消息处理回调函数类型：接受会话指针、消息ID、消息数据
typedef  function<void(shared_ptr<CSession>, const short &msg_id, const string &msg_data)> FunCallBack;

/**
 * @brief 业务逻辑处理系统（单例）
 * 
 * 负责处理所有客户端消息的业务逻辑。采用生产者-消费者模式：
 * - IO线程读取完消息后，通过 PostMsgToQue() 投递到消息队列
 * - 内部工作线程从队列取出消息，根据消息ID分发到对应的Handler处理
 * 
 * 支持的消息处理包括：登录、搜索用户、加好友、认证好友、
 * 文本聊天、图片聊天、心跳、加载会话列表、创建私聊、加载历史消息等。
 */
class LogicSystem:public Singleton<LogicSystem>
{
	friend class Singleton<LogicSystem>;
public:
	/// 析构函数，幂等停机并回收所有 worker 线程
	~LogicSystem();

	/**
	 * @brief 将客户端消息按路由键投递到对应 logic worker 分片（计划1.2）
	 *
	 * routing_key 由调用方传入（通常为 std::hash<int>{}(uid)），本函数取
	 * routing_key % _logic_workers.size() 选定分片，保证同一 uid 的所有
	 * client 请求进入同一条 FIFO。停机或 worker 拒绝时返回 false。
	 * @param routing_key 路由键（调用方应保证对同一 uid 取值一致）
	 * @param msg 封装了会话和消息数据的逻辑节点
	 * @return 成功入队返回 true；服务端停机/拒绝时返回 false（消息未入队、未持久化）
	 */
	bool PostMsgToQue(std::size_t routing_key, std::shared_ptr<LogicNode> msg);

	/**
	 * @brief 把面向某 uid 的旁路任务投递到该 uid 的 logic 分片（计划1.2/1.5）
	 *
	 * 用 std::hash<int>{}(uid) % N 固定分片，使 recipient notify 与该 uid 的
	 * client 请求进入同一 FIFO。停机时返回 false。
	 * @param uid 目标用户ID
	 * @param task 待执行的任务闭包
	 * @return 成功入队返回 true；服务端停机返回 false
	 */
	bool PostToUser(int uid, LogicWorker::Task task);

	/**
	 * @brief 把会阻塞的跨服同步 gRPC 投递到 sender uid 固定的 delivery 分片（计划1.2）
	 *
	 * 用 std::hash<int>{}(sender_uid) % N 固定到 _delivery_workers，避免 3 秒
	 * deadline 阻塞 logic 分片，也避免同一 sender 的两次跨服调用互相越序。
	 * @param sender_uid 发送者用户ID
	 * @param task 待执行的任务闭包
	 * @return 成功入队返回 true；服务端停机返回 false
	 */
	bool PostDelivery(int sender_uid, LogicWorker::Task task);

	/**
	 * @brief 幂等停机：先拒绝新投递，再排空/join logic workers，最后排空/join delivery workers
	 *
	 * 多次调用安全。logic workers 排空过程中仍可产生 outbound 任务。
	 */
	void Stop();

	/**
	 * @brief 设置CServer实例，使逻辑层能够访问会话管理功能
	 * @param pserver CServer共享指针
	 */
	void SetServer(std::shared_ptr<CServer> pserver);

private:
	/// 私有构造函数，启动工作线程并注册所有消息回调
	LogicSystem();

	/**
	 * @brief 在 logic worker 分片上分发单条客户端消息（计划1.2/1.4）
	 *
	 * 登录一次性：已登录后再收 MSG_CHAT_LOGIN 一律回错误并关闭；
	 * 非登录包先验证 session->GetUserId()==session->GetRoutingUid()，
	 * 不一致（含登录失败后排队）回 UidInvalid 而不执行 handler。
	 * @param msg 待分发的逻辑节点
	 */
	void DispatchClientMessage(std::shared_ptr<LogicNode> msg);

	/// 注册所有消息ID到对应Handler的回调映射
	void RegisterCallBacks();

	/**
	 * @brief 处理用户登录请求（验证Token、分配会话、踢人逻辑等）
	 */
	void LoginHandler(shared_ptr<CSession> session, const short &msg_id, const string &msg_data);

	/**
	 * @brief 处理用户搜索请求（按uid或用户名搜索）
	 */
	void SearchInfo(std::shared_ptr<CSession> session, const short& msg_id, const string& msg_data);

	/**
	 * @brief 处理添加好友申请（写入数据库、通知目标用户）
	 */
	void AddFriendApply(std::shared_ptr<CSession> session, const short& msg_id, const string& msg_data);

	/**
	 * @brief 处理好友认证请求（同意/拒绝，更新数据库，通知申请者）
	 */
	void AuthFriendApply(std::shared_ptr<CSession> session, const short& msg_id, const string& msg_data);

	/**
	 * @brief 处理文本聊天消息（存储、转发给接收者，支持跨服转发）
	 */
	void DealChatTextMsg(std::shared_ptr<CSession> session, const short& msg_id, const string& msg_data);

	/**
	 * @brief 处理客户端心跳请求，更新会话心跳时间并回复
	 */
	void HeartBeatHandler(std::shared_ptr<CSession> session, const short& msg_id, const string& msg_data);

	/**
	 * @brief 判断字符串是否为纯数字（用于区分uid搜索和用户名搜索）
	 * @param str 待判断的字符串
	 * @return true表示为纯数字
	 */
	bool isPureDigit(const std::string& str);

	/**
	 * @brief 根据用户ID查询用户信息并填充到JSON
	 * @param uid_str 用户ID字符串
	 * @param rtvalue [out] 输出的JSON结果
	 */
	void GetUserByUid(std::string uid_str, json& rtvalue);

	/**
	 * @brief 根据用户名查询用户信息并填充到JSON
	 * @param name 用户名
	 * @param rtvalue [out] 输出的JSON结果
	 */
	void GetUserByName(std::string name, json& rtvalue);

	/**
	 * @brief 从Redis获取用户基本信息
	 * @param base_key Redis键前缀
	 * @param uid 用户ID
	 * @param userinfo [out] 输出的用户信息
	 * @return 是否成功获取
	 */
	bool GetBaseInfo(std::string base_key, int uid, std::shared_ptr<UserInfo> &userinfo);

	/**
	 * @brief 获取指定用户的好友申请列表
	 * @param to_uid 目标用户ID
	 * @param list [out] 输出的申请信息列表
	 * @return 是否成功获取
	 */
	bool GetFriendApplyInfo(int to_uid, std::vector<std::shared_ptr<ApplyInfo>>& list);

	/**
	 * @brief 获取指定用户的好友列表
	 * @param self_id 用户ID
	 * @param user_list [out] 输出的好友信息列表
	 * @return 是否成功获取
	 */
	bool GetFriendList(int self_id, std::vector<std::shared_ptr<UserInfo>> & user_list);

	/**
	 * @brief 处理加载用户聊天会话列表请求（分页）
	 */
	void GetUserThreadsHandler(std::shared_ptr<CSession> session, const short& msg_id, const string& msg_data);

	/**
	 * @brief 从数据库分页查询用户的聊天会话列表
	 * @param userId 用户ID
	 * @param lastId 上一页最后一条记录的ID（游标）
	 * @param pageSize 每页数量
	 * @param threads [out] 输出的会话列表
	 * @param loadMore [out] 是否还有更多数据
	 * @param nextLastId [out] 下一页游标
	 * @return 是否成功查询
	 */
	bool GetUserThreads(int64_t userId,
		int64_t lastId,
		int      pageSize,
		std::vector<std::shared_ptr<ChatThreadInfo>>& threads,
		bool& loadMore,
		int& nextLastId);

	/**
	 * @brief 处理创建私聊会话请求
	 */
	void CreatePrivateChat(std::shared_ptr<CSession> session, const short& msg_id, const string& msg_data);

	/**
	 * @brief 处理加载历史聊天消息请求（分页）
	 */
	void LoadChatMsg(std::shared_ptr<CSession> session, const short& msg_id, const string& msg_data);

	/**
	 * @brief 处理图片聊天消息（存储记录、通知ResourceServer上传、转发给接收者）
	 */
	void DealChatImgMsg(std::shared_ptr<CSession> session, const short& msg_id, const string& msg_data);
	
	/// 停机标志：置 true 后 PostMsgToQue/PostToUser/PostDelivery 立即拒绝新投递
	std::atomic<bool> _stopping{false};
	/// 保护 Stop() 幂等执行（只一次排空/join）的互斥锁
	std::mutex _stop_mutex;
	/// 按 uid 分片的逻辑处理 worker 池（客户端消息 + 面向 uid 的入站通知）
	std::vector<std::unique_ptr<LogicWorker>> _logic_workers;
	/// 按 sender uid 分片的跨服投递 worker 池（仅执行会阻塞的同步 gRPC）
	std::vector<std::unique_ptr<LogicWorker>> _delivery_workers;
	/// 消息回调映射表，构造时注册后只读
	std::map<short, FunCallBack> _fun_callbacks;
	/// CServer实例指针，用于访问在线会话和踢人等操作
	std::shared_ptr<CServer> _p_server;
};

