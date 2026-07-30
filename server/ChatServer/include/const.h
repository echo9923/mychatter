#pragma once
#include <functional>

/**
 * @brief 服务端统一错误码枚举
 * 
 * 用于各服务间通信及向客户端返回错误信息，
 * 客户端根据错误码展示对应的提示信息。
 */
enum ErrorCodes {
	Success = 0,          ///< 操作成功
	Error_Json = 1001,    ///< JSON解析错误
	RPCFailed = 1002,     ///< RPC请求失败
	VarifyExpired = 1003, ///< 验证码已过期
	VarifyCodeErr = 1004, ///< 验证码错误
	UserExist = 1005,     ///< 用户已存在（注册时）
	PasswdErr = 1006,     ///< 密码错误
	EmailNotMatch = 1007, ///< 邮箱不匹配
	PasswdUpFailed = 1008,  ///< 更新密码失败
	PasswdInvalid = 1009,   ///< 密码无效/更新失败
	TokenInvalid = 1010,    ///< Token失效（登录凭证过期）
	UidInvalid = 1011,      ///< 用户ID无效
	CREATE_CHAT_FAILED = 1012, ///< 创建聊天会话失败
	LOAD_CHAT_FAILED = 1013,   ///< 加载聊天记录失败
};


/**
 * @brief 延迟执行工具类（RAII风格）
 * 
 * 在构造时接受一个可调用对象（lambda/函数指针），
 * 在析构时自动执行该函数。常用于确保资源释放、连接归还等操作
 * 不会因异常或提前返回而被遗漏。
 */
class Defer {
public:
	/**
	 * @brief 构造函数，接受一个延迟执行的函数
	 * @param func 要在析构时执行的无参无返回值函数
	 */
	Defer(std::function<void()> func) : func_(func) {}

	/// 析构函数，执行传入的延迟函数
	~Defer() {
		func_();
	}

private:
	/// 存储待延迟执行的函数对象
	std::function<void()> func_;
};

/// 单次接收/发送缓冲区的最大字节数 (2KB)
#define MAX_LENGTH  1024*2
/// 消息头部总长度（字节），包含消息ID和数据长度
#define HEAD_TOTAL_LEN 4
/// 消息头部中消息ID占用的字节数
#define HEAD_ID_LEN 2
/// 消息头部中数据长度字段占用的字节数
#define HEAD_DATA_LEN 2
/// 接收队列最大容量，超过此值将拒绝接收新消息
#define MAX_RECVQUE  10000
/// 发送队列最大容量，超过此值将拒绝发送新消息
#define MAX_SENDQUE 1000


/**
 * @brief TCP消息ID枚举
 * 
 * 定义客户端与ChatServer之间所有TCP消息的类型编号。
 * 消息协议格式：[2字节消息ID][2字节数据长度][数据体]
 */
enum MSG_IDS {
	MSG_CHAT_LOGIN = 1005,          ///< 用户登录请求
	MSG_CHAT_LOGIN_RSP = 1006,      ///< 用户登录响应
	ID_SEARCH_USER_REQ = 1007,      ///< 搜索用户请求（按uid或用户名）
	ID_SEARCH_USER_RSP = 1008,      ///< 搜索用户响应
	ID_ADD_FRIEND_REQ = 1009,       ///< 申请添加好友请求
	ID_ADD_FRIEND_RSP  = 1010,      ///< 申请添加好友响应
	ID_NOTIFY_ADD_FRIEND_REQ = 1011, ///< 服务端通知目标用户收到好友申请
	ID_AUTH_FRIEND_REQ = 1013,      ///< 认证好友请求（同意/拒绝）
	ID_AUTH_FRIEND_RSP = 1014,      ///< 认证好友响应
	ID_NOTIFY_AUTH_FRIEND_REQ = 1015, ///< 服务端通知申请者认证结果
	ID_TEXT_CHAT_MSG_REQ = 1017,    ///< 发送文本聊天消息请求
	ID_TEXT_CHAT_MSG_RSP = 1018,    ///< 文本聊天消息发送响应
	ID_NOTIFY_TEXT_CHAT_MSG_REQ = 1019, ///< 服务端通知接收者收到文本消息
	ID_NOTIFY_OFF_LINE_REQ = 1021,  ///< 服务端通知用户被踢下线
	ID_HEART_BEAT_REQ = 1023,       ///< 客户端心跳请求
	ID_HEARTBEAT_RSP = 1024,        ///< 服务端心跳响应
	ID_LOAD_CHAT_THREAD_REQ = 1025, ///< 加载聊天会话列表请求
	ID_LOAD_CHAT_THREAD_RSP = 1026, ///< 加载聊天会话列表响应
	ID_CREATE_PRIVATE_CHAT_REQ = 1027, ///< 创建私聊会话请求
	ID_CREATE_PRIVATE_CHAT_RSP = 1028, ///< 创建私聊会话响应
	ID_LOAD_CHAT_MSG_REQ = 1029,    ///< 加载历史聊天消息请求（分页）
	ID_LOAD_CHAT_MSG_RSP = 1030,    ///< 加载历史聊天消息响应
	ID_IMG_CHAT_MSG_REQ = 1035,     ///< 发送图片聊天消息请求
	ID_IMG_CHAT_MSG_RSP = 1036,     ///< 图片聊天消息发送响应
	ID_NOTIFY_IMG_CHAT_MSG_REQ = 1039, ///< 服务端通知接收者收到图片消息
	ID_FILE_INFO_SYNC_REQ = 1041,   ///< 文件信息同步请求（断点续传）
	ID_FILE_INFO_SYNC_RSP = 1042    ///< 文件信息同步响应
};

/// Redis中存储用户IP地址的键前缀，完整键为 "uip_" + uid
#define USERIPPREFIX  "uip_"
/// Redis中存储用户登录Token的键前缀，完整键为 "utoken_" + uid
#define USERTOKENPREFIX  "utoken_"
/// Redis中存储各ChatServer节点IP连接数的键前缀，用于负载均衡
#define IPCOUNTPREFIX  "ipcount_"
/// Redis中存储用户基本信息的Hash键前缀，完整键为 "ubaseinfo_" + uid
#define USER_BASE_INFO "ubaseinfo_"
/// Redis中存储登录计数器的键名，用于负载均衡统计
#define LOGIN_COUNT  "logincount"
/// Redis中存储用户名到uid映射的键前缀，完整键为 "nameinfo_" + name
#define NAME_INFO  "nameinfo_"
/// Redis中分布式锁的键前缀，完整键为 "lock_" + 资源名
#define LOCK_PREFIX "lock_"
/// Redis中存储用户会话ID的键前缀，用于踢人逻辑判断
#define USER_SESSION_PREFIX "usession_"
/// Redis中存储分布式锁计数器的键名
#define LOCK_COUNT "lockcount"

/// 分布式锁的持有超时时间（秒），超时后锁自动释放防止死锁
#define LOCK_TIME_OUT 10
/// 分布式锁的获取重试超时时间（秒），超过此时间未获取到锁则放弃
#define ACQUIRE_TIME_OUT 5

/**
 * @brief 聊天消息状态枚举
 * 
 * 表示一条聊天消息当前的发送/阅读状态。
 */
enum MsgStatus {
	UN_READ = 0,     ///< 对方未读
	SEND_FAILED = 1, ///< 发送失败
	READED = 2,      ///< 对方已读
	UN_UPLOAD = 3    ///< 资源未上传完成（如图片还在上传中）
};


