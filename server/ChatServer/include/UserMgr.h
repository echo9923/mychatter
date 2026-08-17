#pragma once
#include "Singleton.h"
#include <unordered_map>
#include <memory>
#include <mutex>

class CSession;

/**
 * @brief 用户会话管理器（单例）
 * 
 * 维护当前 ChatServer 上所有在线用户的 uid 到 Session 的映射关系。
 * 用于：
 * - 根据uid查找在线用户的会话（向在线用户推送消息）
 * - 用户登录时绑定uid与session
 * - 用户下线/被踢时移除映射
 */
class UserMgr: public Singleton<UserMgr>
{
	friend class Singleton<UserMgr>;
public:
	/// 析构函数
	~UserMgr();

	/**
	 * @brief 根据用户ID获取其在线会话
	 * @param uid 用户ID
	 * @return 会话共享指针，若用户不在线则返回nullptr
	 */
	std::shared_ptr<CSession> GetSession(int uid);

	/**
	 * @brief 绑定用户ID与会话（用户登录成功后调用）
	 * @param uid 用户ID
	 * @param session 对应的会话对象
	 */
	void SetUserSession(int uid, std::shared_ptr<CSession> session);

	/**
	 * @brief 移除用户ID与会话的绑定（用户下线/被踢时调用）
	 * @param uid 用户ID
	 * @param session 预期移除的会话对象（防止延迟清理误删新会话）
	 */
	void RmvUserSession(int uid, const std::shared_ptr<CSession>& session);

private:
	/// 私有构造函数
	UserMgr();
	/// 会话映射互斥锁，保护_uid_to_session的线程安全
	std::mutex _session_mtx;
	/// uid到会话的映射表，存储当前服务器上所有在线用户的会话
	std::unordered_map<int, std::shared_ptr<CSession>> _uid_to_session;
};
