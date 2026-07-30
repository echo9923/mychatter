#pragma once
#include "Singleton.h"
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
	/// 析构函数，停止工作线程并清理资源
	~LogicSystem();

	/**
	 * @brief 将消息节点投递到业务处理队列（由IO线程调用）
	 * @param msg 封装了会话和消息数据的逻辑节点
	 */
	void PostMsgToQue(shared_ptr < LogicNode> msg);

	/**
	 * @brief 设置CServer实例，使逻辑层能够访问会话管理功能
	 * @param pserver CServer共享指针
	 */
	void SetServer(std::shared_ptr<CServer> pserver);

private:
	/// 私有构造函数，启动工作线程并注册所有消息回调
	LogicSystem();

	/// 工作线程主函数，循环从消息队列取出消息并分发处理
	void DealMsg();

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
	
	/// 业务工作线程，负责从消息队列取出消息并处理
	std::thread _worker_thread;
	/// 消息队列，存储待处理的客户端消息节点
	std::queue<shared_ptr<LogicNode>> _msg_que;
	/// 消息队列互斥锁，保护队列的线程安全
	std::mutex _mutex;
	/// 条件变量，当队列为空时工作线程阻塞等待，有新消息时唤醒
	std::condition_variable _consume;
	/// 停止标志，为true时工作线程退出循环
	bool _b_stop;
	/// 消息回调映射表，键为消息ID，值为对应的处理函数
	std::map<short, FunCallBack> _fun_callbacks;
	/// CServer实例指针，用于访问在线会话和踢人等操作
	std::shared_ptr<CServer> _p_server;
};

