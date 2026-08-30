#include "userdata.h"
#include <memory>

SearchInfo::SearchInfo(int uid, QString name,
    QString nick, QString desc, int sex, QString icon):_uid(uid)
  ,_name(name), _nick(nick),_desc(desc),_sex(sex),_icon(icon){
}

AddFriendApply::AddFriendApply(int from_uid, QString name, QString desc,
	QString icon, QString nick, int sex, qint64 friend_request_id)
	:_from_uid(from_uid),_friend_request_id(friend_request_id),_name(name),
      _desc(desc),_icon(icon),_nick(nick),_sex(sex)
{

}

ChatDataBase::ChatDataBase(qint64 msg_id, qint64 thread_id,
    ChatMsgType msg_type, QString content, int send_uid, int status):_msg_id(msg_id),
_thread_id(thread_id),
_msg_type(msg_type), _content(content), _send_uid(send_uid), _status(status){

}

ChatDataBase::ChatDataBase(QString client_message_id, qint64 thread_id,
    ChatMsgType msg_type, QString content, int send_uid, int status)
    : _client_message_id(client_message_id), _thread_id(thread_id),
      _msg_type(msg_type), _content(content), _send_uid(send_uid), _status(status)
{

}

ChatDataBase::ChatDataBase(qint64 msg_id, QString client_message_id, qint64 thread_id, ChatMsgType msg_type,
    QString content, int send_uid, int status):_client_message_id(client_message_id), _msg_id(msg_id),
    _thread_id(thread_id),
    _msg_type(msg_type), _content(content), _send_uid(send_uid), _status(status) {

}

void ChatThreadData::AddMsg(std::shared_ptr<ChatDataBase> msg)
{
    _msg_map.insert(msg->GetMsgId(), msg); 
    _last_msg = msg->GetMsgContent();
    _last_msg_id = msg->GetMsgId();
}

void ChatThreadData::MoveMsg(std::shared_ptr<ChatDataBase> msg) {

    auto iter = _msg_unrsp_map.find(msg->GetClientMessageId());
    if (iter == _msg_unrsp_map.end()) {
        AddMsg(msg);
        return;
    }
   
    iter.value()->SetMsgId(msg->GetMsgId());
    iter.value()->SetStatus(msg->GetStatus());
    AddMsg(iter.value());
    _msg_unrsp_map.erase(iter);
}

void ChatThreadData::UpdateProgress(std::shared_ptr<MsgInfo> msg) {
    auto iter = _msg_map.find(msg->_msg_id);
    if (iter == _msg_map.end()) {
        return;
    }

    //更新进度信息,根据消息类型转化为具体类型
    if (msg->_msg_type == MsgType::IMG_MSG) {
        auto img_chat_data = std::dynamic_pointer_cast<ImgChatData>(iter.value());
        img_chat_data->_msg_info->_rsp_size = msg->_rsp_size;
        img_chat_data->_msg_info->_current_size = msg->_current_size;
    }
}

void ChatThreadData::SetLastMsgId(qint64 msg_id)
{
    _last_msg_id = msg_id;
}

int ChatThreadData::GetPeerUserId() {
    return _peer_user_id;
}

qint64 ChatThreadData::GetThreadId()
{
    return _thread_id;
}

QMap<qint64, std::shared_ptr<ChatDataBase>>& ChatThreadData::GetMsgMapRef()
{
    return _msg_map;
}


void ChatThreadData::AppendMsg(qint64 msg_id, std::shared_ptr<ChatDataBase> base_msg) {
    _msg_map.insert(msg_id, base_msg);
    _last_msg = base_msg->GetMsgContent();
    _last_msg_id = msg_id;
}

void ChatThreadData::ClearMsgs() {
    _msg_map.clear();
}

bool ChatThreadData::ContainsMessage(qint64 msg_id) {
    return _msg_map.contains(msg_id);
}

QString ChatThreadData::GetLastMsg()
{
    return _last_msg;
}

qint64 ChatThreadData::GetLastMsgId()
{
    return _last_msg_id;
}

void ChatThreadData::AppendUnRspMsg(QString client_message_id, std::shared_ptr<ChatDataBase> base_msg)
{
    _msg_unrsp_map.insert(client_message_id, base_msg);
}


QMap<QString, std::shared_ptr<ChatDataBase>>& ChatThreadData::GetMsgUnRspRef() {
    return _msg_unrsp_map;
}

std::shared_ptr<ChatDataBase> ChatThreadData::GetChatDataBase(qint64 msg_id) {
    auto iter = _msg_map.find(msg_id);
    if (iter == _msg_map.end()) {
        return nullptr;
    }

    return iter.value();
}
