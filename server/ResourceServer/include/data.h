#pragma once
#include <string>
#include <vector>
struct UserInfo {
	UserInfo():name(""),uid(0),email(""),nick(""),desc(""),sex(0), icon(""), back("") {}
	std::string name;
	int uid;
	std::string email;
	std::string nick;
	std::string desc;
	int sex;
	std::string icon;
	std::string back;
};

struct ApplyInfo {
	ApplyInfo(int uid, std::string name, std::string desc,
		std::string icon, std::string nick, int sex, int status)
		:_uid(uid),_name(name),_desc(desc),
		_icon(icon),_nick(nick),_sex(sex),_status(status){}

	int _uid;
	std::string _name;
	std::string _desc;
	std::string _icon;
	std::string _nick;
	int _sex;
	int _status;
};

//聊天线程信息
struct ChatThreadInfo {
	long long _thread_id;  // 64 位：thread_id 全链路 >32 位不截断
	std::string _type;     // "private" or "group"
	int _user1_id;    // 私聊时对应 private_chat.user1_id；群聊时设为 0
	int _user2_id;    // 私聊时对应 private_chat.user2_id；群聊时设为 0
};

//聊天消息信息
struct ChatMessage {
	long long message_id;  // 64 位：chat_message.message_id 为 BIGINT UNSIGNED
	long long thread_id;
	unsigned long long recv_seq; // 接收者维度连续序号；上传完成前为 0/NULL
	int sender_id;
	int recv_id;
	std::string unique_id;
	std::string content;      // 资源消息为原始文件名（仅展示；磁盘文件以 message_id 命名）
	std::string chat_time;
	int status;               // 纯阅读态（0未读/1发送失败/2已读；3已废弃）
	int msg_type;             // 0文本 1图片 3文件（2 已废弃）
	int resource_status;      // 资源生命周期（0待上传/1就绪/2失败过期，仅 msg_type 1/3 有意义）
	unsigned long long content_size; // 内容字节大小（资源为字节数）
	std::string content_hash; // 整文件 SHA-256 小写 hex（资源消息）
	std::string mime_type;    // 资源 MIME 类型（如 image/png）
	int business_status;      // 普通资源固定为 0
	long long related_message_id;
	std::string handled_at;
	std::string requester_remark;
};

//过期资源查询结果行：清理任务用
struct ExpiredResource {
	long long message_id;
	int sender_id;
	int recv_id;
};

// 查询结果结构，增加next_cursor字段
struct PageResult {
	std::vector<ChatMessage> messages;
	bool load_more;
	long long next_cursor;  // 本页最后一条message_id，用于下次查询
};

