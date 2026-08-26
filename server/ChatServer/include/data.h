#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <utility>

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
	ApplyInfo(std::int64_t message_id, int from_uid, int to_uid,
		std::string name, std::string desc, std::string requester_remark,
		std::string icon, std::string nick, int sex, int status)
		:_message_id(message_id), _from_uid(from_uid), _to_uid(to_uid),
		_uid(from_uid), _name(std::move(name)), _desc(std::move(desc)),
		_requester_remark(std::move(requester_remark)), _icon(std::move(icon)),
		_nick(std::move(nick)), _sex(sex), _status(status){}

	std::int64_t _message_id;
	int _from_uid;
	int _to_uid;
	int _uid;           ///< 申请者用户ID
	std::string _name;  ///< 申请者用户名
	std::string _desc;  ///< 申请附言/描述
	std::string _requester_remark; ///< 申请人希望给对方设置的备注
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
	std::int64_t _thread_id{0};  ///< 会话线程ID（主键，64 位）
	std::string _type;  ///< 会话类型: "private"(私聊) 或 "group"(群聊)
	int _user1_id;      ///< 私聊时对应 user1_id；群聊时设为 0
	int _user2_id;      ///< 私聊时对应 user2_id；群聊时设为 0
};

/**
 * @brief 资源生命周期状态枚举
 *
 * 与数据库 resource_status 列对应，图片/文件统一资源消息的单一真值。
 * status 的 3=UN_UPLOAD 语义已废弃：资源是否可下载只看本枚举，
 * status 收紧为纯阅读态（0 未读/1 发送失败/2 已读）。
 */
enum class ResourceStatus {
	Uploading = 0, ///< 待上传（1503 创建后，分片未收齐）
	Ready     = 1, ///< 就绪（分片收齐、整文件 SHA-256 校验通过、已进同步流）
	Expired   = 2  ///< 失败/过期（7 天清理标记或整文件校验失败终态）
};

/// 好友申请业务状态；普通聊天和资源消息固定为 None。
enum class BusinessStatus {
	None = 0,
	Pending = 1,
	Accepted = 2,
	Rejected = 3
};

/**
 * @brief 聊天消息结构体
 * 
 * 对应数据库中的一条聊天消息记录。
 */
struct ChatMessage {
	std::int64_t message_id{0}; ///< 消息ID（主键，自增，64 位）
	std::int64_t thread_id{0};  ///< 所属会话线程ID（64 位）
	std::uint64_t recv_seq{0};  ///< 接收者维度连续序号；未发布资源为 0/NULL
	int sender_id;          ///< 发送者用户ID
	int recv_id;            ///< 接收者用户ID
	std::string unique_id;  ///< 消息唯一标识（客户端生成，用于去重，历史/系统消息为空串）
	std::string content;    ///< 消息内容（文本或图片URL）
	std::string chat_time;  ///< 消息发送时间
	int status;             ///< 消息状态（纯阅读态：0未读/1发送失败/2已读；3已废弃）
	int msg_type;           ///< 消息类型（参见ChatMsgType枚举）
	ResourceStatus resource_status{ResourceStatus::Uploading}; ///< 资源生命周期（仅 msg_type 1/3 有意义）
	std::uint64_t content_size{0};                ///< 内容字节大小（文本为0，资源为字节数）
	std::string content_hash;                     ///< 整文件 SHA-256 小写 hex（资源消息必填，文本为空）
	std::string mime_type;                         ///< 资源 MIME 类型（如 image/png，仅展示用）
	BusinessStatus business_status{BusinessStatus::None}; ///< 好友申请业务状态
	std::int64_t related_message_id{0}; ///< 好友同意/拒绝结果关联的申请消息
	std::string handled_at;             ///< 好友申请处理时间
	std::string requester_remark;       ///< 好友申请人给目标用户设置的备注
};

/**
 * @brief 增量同步结果项
 *
 * chat_message 的一行；recv_seq 在同一接收者下严格连续。
 */
struct SyncedMessage {
	std::uint64_t recv_seq{0};              ///< 接收者维度连续序号
	std::shared_ptr<ChatMessage> msg;       ///< 消息本体
};

/**
 * @brief 分页查询结果结构体
 * 
 * 用于聊天消息的分页加载，支持基于游标的向下滚动加载。
 */
struct PageResult {
	std::vector<ChatMessage> messages; ///< 本页查询到的消息列表
	bool load_more;                    ///< 是否还有更多数据可加载
	std::int64_t next_cursor{0};       ///< 下一页的游标（本页最后一条message_id）
};

/**
 * @brief 聊天消息类型枚举
 * 
 * 区分不同媒体类型的聊天消息。
 */
enum class ChatMsgType {
	TEXT = 0,   ///< 文本消息
	PIC = 1,    ///< 图片消息
	FILE = 3,   ///< 文件消息
	FRIEND_APPLY = 10,
	FRIEND_ACCEPT = 11,
	FRIEND_REJECT = 12
};
