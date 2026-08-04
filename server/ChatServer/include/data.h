#pragma once
#include <string>
#include <vector>
#include <cstdint>

/**
 * @brief 用户基本信息结构体
 * 
 * 存储一个用户的完整资料信息，对应数据库 user 表的字段。
 * 用于登录验证、用户搜索、好友列表展示等场景。
 */
struct UserInfo {
	/// 默认构造函数，初始化所有字段为空/0
	UserInfo():name(""),uid(0),email(""),nick(""),desc(""),sex(0), icon(""), back("") {}
	std::string name;   ///< 用户名（登录账号）
	int uid;            ///< 用户唯一ID
	std::string email;  ///< 邮箱地址
	std::string nick;   ///< 昵称
	std::string desc;   ///< 个人描述/签名
	int sex;            ///< 性别（0:未设置, 1:男, 2:女）
	std::string icon;   ///< 头像图片URL
	std::string back;   ///< 背景图片URL
};

/**
 * @brief 好友申请信息结构体
 * 
 * 存储一条好友申请的详细信息，用于展示好友申请列表。
 */
struct ApplyInfo {
	/**
	 * @brief 构造函数
	 * @param uid 申请者用户ID
	 * @param name 申请者用户名
	 * @param desc 申请附言/描述
	 * @param icon 申请者头像URL
	 * @param nick 申请者昵称
	 * @param sex 申请者性别
	 * @param status 申请状态（0:待处理, 1:已同意, 2:已拒绝）
	 */
	ApplyInfo(int uid, std::string name, std::string desc,
		std::string icon, std::string nick, int sex, int status)
		:_uid(uid),_name(name),_desc(desc),
		_icon(icon),_nick(nick),_sex(sex),_status(status){}

	int _uid;           ///< 申请者用户ID
	std::string _name;  ///< 申请者用户名
	std::string _desc;  ///< 申请附言/描述
	std::string _icon;  ///< 申请者头像URL
	std::string _nick;  ///< 申请者昵称
	int _sex;           ///< 申请者性别
	int _status;        ///< 申请状态（0:待处理, 1:已同意, 2:已拒绝）
};

/**
 * @brief 聊天会话线程信息结构体
 * 
 * 对应数据库中的聊天会话记录，表示两个用户之间的私聊或一个群聊。
 */
struct ChatThreadInfo {
	int _thread_id;     ///< 会话线程ID（主键）
	std::string _type;  ///< 会话类型: "private"(私聊) 或 "group"(群聊)
	int _user1_id;      ///< 私聊时对应 user1_id；群聊时设为 0
	int _user2_id;      ///< 私聊时对应 user2_id；群聊时设为 0
};

/**
 * @brief 消息投递状态枚举
 *
 * 与数据库 delivery_status 列对应，表示应用层“至少一次投递”的进展。
 * 与展示状态(status)分离：status 描述阅读/上传状态，delivery_status 描述是否已被接收方 ACK。
 */
enum class DeliveryStatus {
	Pending = 0, ///< 待投递（新写入默认值，进入离线 pending 集合）
	Acked   = 1  ///< 已投递（接收方已 ACK；历史/系统消息也固定为已投递，不重推）
};

/**
 * @brief 聊天消息结构体
 * 
 * 对应数据库中的一条聊天消息记录。
 */
struct ChatMessage {
	int message_id;         ///< 消息ID（主键，自增）
	int thread_id;          ///< 所属会话线程ID
	int sender_id;          ///< 发送者用户ID
	int recv_id;            ///< 接收者用户ID
	std::string unique_id;  ///< 消息唯一标识（客户端生成，用于去重，历史/系统消息为空串）
	std::string content;    ///< 消息内容（文本或图片URL）
	std::string chat_time;  ///< 消息发送时间
	int status;             ///< 消息状态（参见MsgStatus枚举）
	int msg_type;           ///< 消息类型（参见ChatMsgType枚举）
	std::uint64_t content_size{0};                ///< 内容字节大小（文本为0，图片为字节数）
	DeliveryStatus delivery_status{DeliveryStatus::Pending}; ///< 应用层投递状态（参见DeliveryStatus枚举）
};

/**
 * @brief 分页查询结果结构体
 * 
 * 用于聊天消息的分页加载，支持基于游标的向下滚动加载。
 */
struct PageResult {
	std::vector<ChatMessage> messages; ///< 本页查询到的消息列表
	bool load_more;                    ///< 是否还有更多数据可加载
	int next_cursor;                   ///< 下一页的游标（本页最后一条message_id）
};

/**
 * @brief 聊天消息类型枚举
 * 
 * 区分不同媒体类型的聊天消息。
 */
enum class ChatMsgType {
	TEXT = 0,   ///< 文本消息
	PIC = 1,    ///< 图片消息
	VIDEO = 2,  ///< 视频消息
	FILE = 3    ///< 文件消息
};