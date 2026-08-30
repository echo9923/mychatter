#ifndef USERDATA_H
#define USERDATA_H
#include <QString>
#include <memory>
#include <QJsonArray>
#include <vector>
#include <QJsonObject>
#include <QMap>
#include "global.h"

class SearchInfo {
public:
    SearchInfo(int uid, QString name, QString nick, QString desc, int sex, QString icon);
    SearchInfo() = default;
	int _uid;
	QString _name;
	QString _nick;
	QString _desc;
	int _sex;
    QString _icon;
};

Q_DECLARE_METATYPE(SearchInfo)
Q_DECLARE_METATYPE(std::shared_ptr<SearchInfo>)

class AddFriendApply {
public:
	AddFriendApply(int from_uid, QString name, QString desc,
		QString icon, QString nick, int sex, qint64 friend_request_id = 0);
    AddFriendApply() = default;
	int _from_uid;
	qint64 _friend_request_id = 0;
	QString _name;
	QString _desc;
    QString _icon;
    QString _nick;
    int     _sex;
};

Q_DECLARE_METATYPE(std::shared_ptr<AddFriendApply>)

struct ApplyInfo {
    ApplyInfo() = default;
	ApplyInfo(int uid, QString name, QString desc,
		QString icon, QString nick, int sex, int status, qint64 friend_request_id = 0)
		:_uid(uid),_friend_request_id(friend_request_id),_name(name),_desc(desc),
        _icon(icon),_nick(nick),_sex(sex),_status(status){}

    ApplyInfo(std::shared_ptr<AddFriendApply> addinfo)
		:_uid(addinfo->_from_uid),_friend_request_id(addinfo->_friend_request_id),_name(addinfo->_name),
          _desc(addinfo->_desc),_icon(addinfo->_icon),
          _nick(addinfo->_nick),_sex(addinfo->_sex),
		  _status(static_cast<int>(FriendRequestStatus::PENDING))
    {}
    void SetIcon(QString head){
        _icon = head;
    }
    int _uid;
	qint64 _friend_request_id = 0;
    QString _name;
    QString _desc;
    QString _icon;
    QString _nick;
    int _sex;
    int _status;
};

class TextChatData;
struct AuthInfo {
    AuthInfo(int uid, QString name,
             QString nick, QString icon, int sex):
        _uid(uid), _name(name), _nick(nick), _icon(icon),
        _sex(sex), _thread_id(0){}
    AuthInfo() = default;
    int _uid;
    QString _name;
    QString _nick;
    QString _icon;
    int _sex;
    qint64 _thread_id;
};

Q_DECLARE_METATYPE(std::shared_ptr<AuthInfo>)

struct AuthRsp {
    AuthRsp() = default;
    AuthRsp(int peer_uid, QString peer_name,
            QString peer_nick, QString peer_icon, int peer_sex)
        :_uid(peer_uid),_name(peer_name),_nick(peer_nick),
          _icon(peer_icon),_sex(peer_sex),_thread_id(0)
    {
    
    }


    int _uid;
    QString _name;
    QString _nick;
    QString _icon;
    int _sex;
    qint64 _thread_id;
};

Q_DECLARE_METATYPE(std::shared_ptr<AuthRsp>)

struct UserInfo {
    UserInfo(int uid, QString name, QString nick, QString icon, int sex, QString last_msg = "", QString desc=""):
        _uid(uid),_name(name),_nick(nick),_icon(icon),_sex(sex),_desc(desc){}

    UserInfo(std::shared_ptr<AuthInfo> auth):
        _uid(auth->_uid),_name(auth->_name),_nick(auth->_nick),
        _icon(auth->_icon),_sex(auth->_sex),_desc(""){}

    UserInfo(int uid, QString name, QString icon):
    _uid(uid), _name(name), _icon(icon),_nick(_name),
    _sex(0),_desc(""){

    }

    UserInfo(std::shared_ptr<AuthRsp> auth):
        _uid(auth->_uid),_name(auth->_name),_nick(auth->_nick),
        _icon(auth->_icon),_sex(auth->_sex),_desc(""){}

    UserInfo(std::shared_ptr<SearchInfo> search_info):
        _uid(search_info->_uid),_name(search_info->_name),_nick(search_info->_nick),
    _icon(search_info->_icon),_sex(search_info->_sex), _desc(search_info->_desc){

    }

    UserInfo() = default;

    int _uid;
    QString _name;
    QString _nick;
    QString _icon;
    int _sex;
    QString _desc;
};

Q_DECLARE_METATYPE(std::shared_ptr<UserInfo>)

class ChatDataBase {
public:
    ChatDataBase() = default;
    ChatDataBase(qint64 msg_id, qint64 thread_id, ChatMsgType msg_type,
        QString content,int _send_uid, int status);
    ChatDataBase(QString client_message_id, qint64 thread_id, ChatMsgType msg_type,
        QString content, int send_uid, int status);
    ChatDataBase(qint64 msg_id, QString client_message_id, qint64 thread_id, ChatMsgType msg_type,
        QString content, int send_uid, int status);
    qint64 GetMsgId() { return _msg_id; }
    qint64 GetThreadId() { return _thread_id; }
    ChatMsgType GetMsgType() { return _msg_type; }
    QString GetContent() { return _content; }
    int GetSendUid() { return _send_uid; }
    QString GetMsgContent(){return _content;}
    QString GetClientMessageId() const { return _client_message_id; }
    int GetStatus() { return _status; }
    void SetMsgId(qint64 msg_id) { _msg_id = msg_id; }
    void SetStatus(int status) { _status = status; }
    virtual ~ChatDataBase() {}  // 添加虚析构函数
protected:
    QString _client_message_id;
    qint64 _msg_id = 0;
    qint64 _thread_id = 0;
    ChatMsgType _msg_type = ChatMsgType::TEXT;
    QString _content;
    //发送者id
    int _send_uid = 0;
    //状态
    int _status = 0;
};

Q_DECLARE_METATYPE(std::vector<std::shared_ptr<ChatDataBase>>)

class TextChatData : public ChatDataBase {
public:

    TextChatData(qint64 msg_id, qint64 thread_id, ChatMsgType msg_type, QString content,
        int send_uid, int status) :
        ChatDataBase(msg_id, thread_id, msg_type, content, send_uid, status)
    {

    }

    TextChatData(QString client_message_id, qint64 thread_id, ChatMsgType msg_type, QString content,
        int send_uid, int status) :
        ChatDataBase(client_message_id, thread_id, msg_type, content, send_uid, status)
    {

    }

    TextChatData(qint64 msg_id, QString client_message_id, qint64 thread_id, ChatMsgType msg_type, QString content,
        int send_uid, int status) :
        ChatDataBase(msg_id, client_message_id, thread_id, msg_type, content, send_uid, status)
    {

    }

    TextChatData() = default;

    ~TextChatData() override{}
};

Q_DECLARE_METATYPE(std::vector<std::shared_ptr<TextChatData>>)

class ImgChatData : public ChatDataBase {
public:
    ImgChatData(std::shared_ptr<MsgInfo> msg_info, QString client_message_id,
        qint64 thread_id, ChatMsgType msg_type,
        int send_uid, int status):
        ChatDataBase(client_message_id, thread_id, msg_type, msg_info->_text_or_url,
            send_uid, status), _msg_info(msg_info){
        _msg_id = _msg_info->_msg_id;
    }

    ~ImgChatData() override {}

    std::shared_ptr<MsgInfo> _msg_info;
};

Q_DECLARE_METATYPE(std::shared_ptr<ImgChatData>)

//聊天线程信息
struct ChatThreadInfo {
    qint64 _thread_id;
    int _lower_user_id;
    int _higher_user_id;
    qint64 _last_msg_id = 0;    // 该会话最后一条消息 id（bootstrap 写库用）
    ChatThreadInfo() = default;
};

Q_DECLARE_METATYPE(std::vector<std::shared_ptr<ChatThreadInfo>>)

//客户端本地存储的聊天线程数据结构（只保存当前窗口消息，历史真值在 SQLite）
class ChatThreadData {
public:
    ChatThreadData() = default;
    ChatThreadData(int peer_user_id, qint64 thread_id, qint64 last_msg_id):
        _peer_user_id(peer_user_id), _thread_id(thread_id), _last_msg_id(last_msg_id){}
    void AddMsg(std::shared_ptr<ChatDataBase> msg);
    void MoveMsg(std::shared_ptr<ChatDataBase> msg);
    void UpdateProgress(std::shared_ptr<MsgInfo> msg);
    void SetLastMsgId(qint64 msg_id);
    int  GetPeerUserId();
    qint64  GetThreadId();
    QMap<qint64, std::shared_ptr<ChatDataBase>>&  GetMsgMapRef();
    void AppendMsg(qint64 msg_id, std::shared_ptr<ChatDataBase> base_msg);
    //清空窗口消息（LoadRecentMessages 重载前调用）
    void ClearMsgs();
    //§6.4 recipient 去重：message_id 是否已在 _msg_map 中
    bool ContainsMessage(qint64 msg_id);
    QString GetLastMsg();
    qint64 GetLastMsgId();
    QMap<QString, std::shared_ptr<ChatDataBase>>& GetMsgUnRspRef();
    void AppendUnRspMsg(QString client_message_id, std::shared_ptr<ChatDataBase> base_msg);
    std::shared_ptr<ChatDataBase> GetChatDataBase(qint64 msg_id);
private:
    int _peer_user_id;
    qint64 _last_msg_id;
    qint64 _thread_id;
    QString _last_msg;
    //缓存消息map，抽象为基类，因为会有图片等其他类型消息
    QMap<qint64, std::shared_ptr<ChatDataBase>>  _msg_map;
    //缓存未回复的消息
        //已发送的消息，还未收到回应的。
    QMap<QString, std::shared_ptr<ChatDataBase>> _msg_unrsp_map;
};


#endif
