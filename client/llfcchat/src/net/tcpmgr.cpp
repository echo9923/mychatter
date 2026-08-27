#include "tcpmgr.h"
#include <QAbstractSocket>
#include <QCoreApplication>
#include "usermgr.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonArray>
#include <filetcpmgr.h>
#include <QStandardPaths>
#include "outboxdispatcher.h"
#include "chatsyncmanager.h"
#include <QSet>

//协议中的 64 位 ID 和 recv_seq 只接受十进制字符串。
static qint64 jsonInt64(const QJsonValue& v)
{
    return v.isString() ? v.toString().toLongLong() : 0;
}

TcpMgr::TcpMgr():_host(""),_port(0),_b_recv_pending(false),_message_type(0),_message_len(0),_bytes_sent(0),_pending(false),
    _manual_close(false),_reconnecting(false),_disconnect_notified(false)
{
    registerMetaType();
    QObject::connect(&_socket, &QTcpSocket::connected, this, [&]() {
           qDebug() << "Connected to server!";
           _disconnect_notified = false;
           if (_reconnecting) {
               QJsonObject json_obj;
               json_obj["uid"] = _server_info->_uid;
               json_obj["token"] = _server_info->_token;
               slot_send_data(ID_CHAT_LOGIN,
                              QJsonDocument(json_obj).toJson(QJsonDocument::Compact));
               return;
           }
           emit sig_con_success(true);
       });

       QObject::connect(&_socket, &QTcpSocket::readyRead, this, [&]() {
           // 当有数据可读时，读取所有数据
           // 读取所有数据并追加到缓冲区
           _buffer.append(_socket.readAll());

           forever {
                //先解析头部
               if(!_b_recv_pending){
                   // 检查缓冲区中的数据是否足够解析出一个消息头（消息类型 + 消息长度）
                   if (_buffer.size() < static_cast<int>(sizeof(quint16) * 2)) {
                       return; // 数据不够，等待更多数据
                   }

                   // ✅ 每次都重新创建stream
                   QDataStream stream(_buffer);
                   stream.setVersion(QDataStream::Qt_5_0);
                   stream >> _message_type >> _message_len;
                   _buffer.remove(0, sizeof(quint16) * 2);  // 使用remove代替mid赋值
                   qDebug() << "Message Type:" << _message_type << ", Length:" << _message_len;

               }

                //buffer剩余长读是否满足消息体长度，不满足则退出继续等待接受
               if(_buffer.size() < _message_len){
                    _b_recv_pending = true;
                    return;
               }

               _b_recv_pending = false;
               // 读取消息体
               QByteArray messageBody = _buffer.mid(0, _message_len);
               qDebug() << "receive body msg is " << messageBody ;

               _buffer = _buffer.mid(_message_len);
               handleMsg(ReqId(_message_type),_message_len, messageBody);
           }

       });

       // 处理错误（适用于Qt 5.15之前的版本）
        QObject::connect(&_socket, static_cast<void (QTcpSocket::*)(QTcpSocket::SocketError)>(&QTcpSocket::error),
                            this,
                            [&](QTcpSocket::SocketError socketError) {
               qDebug() << "Error:" << _socket.errorString() ;
               if (_reconnecting) {
                   finishReconnectFailure();
                   return;
               }
               if (_manual_close || _disconnect_notified) {
                   return;
               }
               switch (socketError) {
                   case QTcpSocket::ConnectionRefusedError:
                       qDebug() << "Connection Refused!";
                       emit sig_con_success(false);
                       break;
                   case QTcpSocket::RemoteHostClosedError:
                       qDebug() << "Remote Host Closed Connection!";
                       break;
                   case QTcpSocket::HostNotFoundError:
                       qDebug() << "Host Not Found!";
                       emit sig_con_success(false);
                       break;
                   case QTcpSocket::SocketTimeoutError:
                       qDebug() << "Connection Timeout!";
                       emit sig_con_success(false);
                       break;
                   case QTcpSocket::NetworkError:
                       qDebug() << "Network Error!";
                       break;
                   default:
                       qDebug() << "Other Error!";
                       break;
               }
         });

        // 处理连接断开
        QObject::connect(&_socket, &QTcpSocket::disconnected, this,[&]() {
            qDebug() << "Disconnected from server.";
            if (_manual_close) {
                return;
            }
            if (_reconnecting) {
                finishReconnectFailure();
                return;
            }
            if (!_disconnect_notified) {
                _disconnect_notified = true;
                emit sig_connection_closed();
            }
        });
        //连接发送信号用来发送数据
        QObject::connect(this, &TcpMgr::sig_send_data, this, &TcpMgr::slot_send_data);

        //连接发送信号
        QObject::connect(&_socket, &QTcpSocket::bytesWritten, this, [this](qint64 bytes) {
                     //更新发送数据
                    _bytes_sent += bytes;
                    //未发送完整
                    if (_bytes_sent < _current_block.size()) {
                        //继续发送
                        auto data_to_send = _current_block.mid(_bytes_sent);
                        _socket.write(data_to_send);
                        return;
                    }

                    //发送完全，则查看队列是否为空
                    if (_send_queue.isEmpty()) {
                        //队列为空，说明已经将所有数据发送完成，将pending设置为false，这样后续要发送数据时可以继续发送
                        _current_block.clear();
                        _pending = false;
                        _bytes_sent = 0;
                        return;
                    }

                    //队列不为空，则取出队首元素
                    _current_block = _send_queue.dequeue();
                    _bytes_sent = 0;
                    _pending = true;
                    qint64 w2 = _socket.write(_current_block);
                    qDebug() << "[TcpMgr] Dequeued and write() returned" << w2;
            });


        //关闭socket
        connect(this, &TcpMgr::sig_close, this, &TcpMgr::slot_tcp_close);
        connect(this, &TcpMgr::sig_reconnect_chat, this, &TcpMgr::slot_reconnect_chat);
        //注册消息
        initHandlers();

}

void TcpMgr::registerMetaType() {
    // 注册所有自定义类型
    qRegisterMetaType<ServerInfo>("ServerInfo");
    qRegisterMetaType<SearchInfo>("SearchInfo");
    qRegisterMetaType<std::shared_ptr<SearchInfo>>("std::shared_ptr<SearchInfo>");

    qRegisterMetaType<AddFriendApply>("AddFriendApply");
    qRegisterMetaType<std::shared_ptr<AddFriendApply>>("std::shared_ptr<AddFriendApply>");

    qRegisterMetaType<ApplyInfo>("ApplyInfo");

    qRegisterMetaType<std::shared_ptr<AuthInfo>>("std::shared_ptr<AuthInfo>");

    qRegisterMetaType<AuthRsp>("AuthRsp");
    qRegisterMetaType<std::shared_ptr<AuthRsp>>("std::shared_ptr<AuthRsp>");

    qRegisterMetaType<UserInfo>("UserInfo");

    qRegisterMetaType<std::vector<std::shared_ptr<TextChatData>>>("std::vector<std::shared_ptr<TextChatData>>");

    qRegisterMetaType<std::vector<std::shared_ptr<ChatThreadInfo>>>("std::vector<std::shared_ptr<ChatThreadInfo>>");

    qRegisterMetaType<std::shared_ptr<ChatThreadData>>("std::shared_ptr<ChatThreadData>");
    qRegisterMetaType<ReqId>("ReqId");
    qRegisterMetaType<std::shared_ptr<ImgChatData>>("std::shared_ptr<ImgChatData>");
    qRegisterMetaType<std::vector<std::shared_ptr<ChatDataBase>>>("std::vector<std::shared_ptr<ChatDataBase>>");
    qRegisterMetaType<QList<qint64>>("QList<qint64>");
    qRegisterMetaType<QJsonObject>("QJsonObject");
}

void TcpMgr::CloseConnection(){
    emit sig_close();
}

void TcpMgr::ReconnectChat(const QString& host, quint16 port)
{
    emit sig_reconnect_chat(host, port);
}

TcpMgr::~TcpMgr(){

}

void TcpMgr::initHandlers()
{
    _handlers.insert(ID_CHAT_LOGIN_RSP, &TcpMgr::handleChatLoginRsp);
    _handlers.insert(ID_SEARCH_USER_RSP, &TcpMgr::handleSearchUserRsp);
    _handlers.insert(ID_ADD_FRIEND_RSP, &TcpMgr::handleAddFriendRsp);
	_handlers.insert(ID_HANDLE_FRIEND_RSP, &TcpMgr::handleAuthFriendRsp);
    //1302 文本回包：只解析 JSON 并转发 OutboxDispatcher（可靠语义移交）
    _handlers.insert(ID_TEXT_CHAT_MSG_RSP, &TcpMgr::handleTextChatMsgRsp);
	_handlers.insert(ID_NOTIFY_USER_MESSAGE, &TcpMgr::handleUserMessageNotify);
    _handlers.insert(ID_NOTIFY_OFF_LINE_REQ, &TcpMgr::handleNotifyOfflineReq);
    _handlers.insert(ID_HEARTBEAT_RSP, &TcpMgr::handleHeartbeatRsp);
    _handlers.insert(ID_LOAD_CHAT_THREAD_RSP, &TcpMgr::handleLoadChatThreadRsp);
    _handlers.insert(ID_CREATE_PRIVATE_CHAT_RSP, &TcpMgr::handleCreatePrivateChatRsp);
    _handlers.insert(ID_LOAD_CHAT_MSG_RSP, &TcpMgr::handleLoadChatMsgRsp);
    //1504 资源消息创建回包：只解析 JSON 并转发 OutboxDispatcher（上传启动移交）
    _handlers.insert(ID_CREATE_RESOURCE_MSG_RSP, &TcpMgr::handleCreateResourceMsgRsp);
    //1406 增量同步回包：原始 JSON 对象转发 ChatSyncManager
	_handlers.insert(ID_SYNC_USER_MESSAGE_RSP, &TcpMgr::handleSyncMessageRsp);
}

void TcpMgr::handleChatLoginRsp(ReqId id, int len, QByteArray data)
{
    Q_UNUSED(len);
    qDebug()<< "handle id is "<< id ;
    auto report_login_failure = [this](int error) {
        if (_reconnecting) {
            finishReconnectFailure();
            return;
        }
        emit sig_login_failed(error);
    };
    // 将QByteArray转换为QJsonDocument
    QJsonDocument jsonDoc = QJsonDocument::fromJson(data);

    // 检查转换是否成功
    if(jsonDoc.isNull() || !jsonDoc.isObject()){
       qDebug() << "Failed to create QJsonDocument.";
       report_login_failure(ErrorCodes::ERR_JSON);
       return;
    }

    QJsonObject jsonObj = jsonDoc.object();
    qDebug()<< "data jsonobj is " << jsonObj ;

    if(!jsonObj.contains("error")){
        int err = ErrorCodes::ERR_JSON;
        qDebug() << "Login Failed, err is Json Parse Err" << err ;
        report_login_failure(err);
        return;
    }

    int err = jsonObj["error"].toInt();
    if(err != ErrorCodes::SUCCESS){
        qDebug() << "Login Failed, err is " << err ;
        report_login_failure(err);
        return;
    }

    auto uid = jsonObj["uid"].toInt();
    if (_reconnecting) {
        if (!_server_info || uid != _server_info->_uid) {
            report_login_failure(ErrorCodes::ERR_JSON);
            return;
        }
		const QJsonArray apply_snapshot = jsonObj["apply_list"].toArray();
		const QJsonArray contact_snapshot = jsonObj["friend_list"].toArray();
		QSet<qint64> pending_incoming_ids;
		QSet<int> contact_peer_ids;
		for (const QJsonValue& value : apply_snapshot) {
			const QJsonObject apply = value.toObject();
			if (apply["to_uid"].toInt() == uid && apply["status"].toInt() ==
				static_cast<int>(FriendRequestStatus::PENDING)) {
				pending_incoming_ids.insert(jsonInt64(apply["message_id"]));
			}
		}
		for (const QJsonValue& value : contact_snapshot) {
			const int peer_uid = value.toObject()["uid"].toInt();
			if (peer_uid > 0) contact_peer_ids.insert(peer_uid);
		}
		// Snapshot membership is authoritative for requests handled while offline.
		for (const auto& apply : UserMgr::GetInstance()->GetApplyList()) {
			if (!apply || apply->_status != static_cast<int>(FriendRequestStatus::PENDING)) {
				continue;
			}
			const bool is_contact = contact_peer_ids.contains(apply->_uid);
			if (!is_contact && pending_incoming_ids.contains(apply->_message_id)) {
				continue;
			}
			const auto status = is_contact ? FriendRequestStatus::ACCEPTED
				: FriendRequestStatus::REJECTED;
			UserMgr::GetInstance()->UpdateApplyStatus(apply->_message_id, status);
			emit sig_friend_request_handled(apply->_message_id, static_cast<int>(status));
		}
		for (const QJsonValue& value : apply_snapshot) {
			const QJsonObject apply = value.toObject();
			if (apply["to_uid"].toInt() != uid || apply["status"].toInt() !=
				static_cast<int>(FriendRequestStatus::PENDING)) {
				continue;
			}
			auto incoming = std::make_shared<AddFriendApply>(
				apply["from_uid"].toInt(), apply["name"].toString(),
				apply["desc"].toString(), apply["icon"].toString(),
				apply["nick"].toString(), apply["sex"].toInt(),
				jsonInt64(apply["message_id"]));
			UserMgr::GetInstance()->AddApplyList(std::make_shared<ApplyInfo>(incoming));
			emit sig_friend_apply(incoming);
		}
		for (const QJsonValue& value : contact_snapshot) {
			const QJsonObject contact = value.toObject();
			const int peer_uid = contact["uid"].toInt();
			if (peer_uid <= 0) continue;
			if (UserMgr::GetInstance()->CheckFriendById(peer_uid)) continue;
			auto auth = std::make_shared<AuthInfo>(peer_uid,
				contact["name"].toString(), contact["nick"].toString(),
				contact["icon"].toString(), contact["sex"].toInt());
			auth->_thread_id = jsonInt64(contact["thread_id"]);
			emit sig_add_auth_friend(auth);
		}

        _reconnecting = false;
        _manual_close = false;
        _disconnect_notified = false;
		emit sig_login_snapshot(jsonObj, false);
		//重连成功：恢复 outbox 派发与增量同步
        emit sig_chat_login_ready();
        emit sig_reconnect_finished(true);
        return;
    }

    auto name = jsonObj["name"].toString();
    auto nick = jsonObj["nick"].toString();
    auto icon = jsonObj["icon"].toString();
    auto sex = jsonObj["sex"].toInt();
    auto desc = jsonObj["desc"].toString();
    auto user_info = std::make_shared<UserInfo>(uid, name, nick, icon, sex,"",desc);

    UserMgr::GetInstance()->SetUserInfo(user_info);
    //Chat 登录成功后用 Gate 下发的统一 token（_server_info 持有）作为 Resource 鉴权凭据
    UserMgr::GetInstance()->SetToken(_server_info->_token);
    if(jsonObj.contains("apply_list")){
        UserMgr::GetInstance()->AppendApplyList(jsonObj["apply_list"].toArray());
    }

    //添加好友列表
    if (jsonObj.contains("friend_list")) {
        UserMgr::GetInstance()->AppendFriendList(jsonObj["friend_list"].toArray());
    }

	emit sig_login_snapshot(jsonObj, true);
	//登录成功：启动 outbox 派发与增量同步
    emit sig_chat_login_ready();

    //3.2 Chat 认证成功后触发 FileTcpMgr 连接 Resource（UI 切换延迟到 Resource 鉴权成功）
    emit sig_connect_resource(_server_info);
}

void TcpMgr::handleSearchUserRsp(ReqId id, int len, QByteArray data)
{
    Q_UNUSED(len);
    qDebug() << "handle id is " << id << " data is " << data;
    // 将QByteArray转换为QJsonDocument
    QJsonDocument jsonDoc = QJsonDocument::fromJson(data);

    // 检查转换是否成功
    if (jsonDoc.isNull()) {
        qDebug() << "Failed to create QJsonDocument.";
        return;
    }

    QJsonObject jsonObj = jsonDoc.object();

    if (!jsonObj.contains("error")) {
        int err = ErrorCodes::ERR_JSON;
        qDebug() << "Login Failed, err is Json Parse Err" << err;

        emit sig_user_search(nullptr);
        return;
    }

    int err = jsonObj["error"].toInt();
    if (err != ErrorCodes::SUCCESS) {
        qDebug() << "Login Failed, err is " << err;
        emit sig_user_search(nullptr);
        return;
    }
   auto search_info =  std::make_shared<SearchInfo>(jsonObj["uid"].toInt(), jsonObj["name"].toString(),
        jsonObj["nick"].toString(), jsonObj["desc"].toString(),
           jsonObj["sex"].toInt(), jsonObj["icon"].toString());

    emit sig_user_search(search_info);
}

void TcpMgr::handleAddFriendRsp(ReqId id, int len, QByteArray data)
{
    Q_UNUSED(len);
    qDebug() << "handle id is " << id << " data is " << data;
    // 将QByteArray转换为QJsonDocument
    QJsonDocument jsonDoc = QJsonDocument::fromJson(data);

    // 检查转换是否成功
    if (jsonDoc.isNull()) {
        qDebug() << "Failed to create QJsonDocument.";
        return;
    }

    QJsonObject jsonObj = jsonDoc.object();

    if (!jsonObj.contains("error")) {
        int err = ErrorCodes::ERR_JSON;
        qDebug() << "Add Friend Failed, err is Json Parse Err" << err;
        return;
    }

	int err = jsonObj["error"].toInt();
	emit sig_user_message_business_rsp(jsonObj);
	if (err != ErrorCodes::SUCCESS) {
		qDebug() << "Add Friend Failed, err is " << err;
        return;
    }

     qDebug() << "Add Friend Success " ;
}

void TcpMgr::handleAuthFriendRsp(ReqId id, int len, QByteArray data)
{
    Q_UNUSED(len);
    qDebug() << "handle id is " << id << " data is " << data;
    // 将QByteArray转换为QJsonDocument
    QJsonDocument jsonDoc = QJsonDocument::fromJson(data);

    // 检查转换是否成功
    if (jsonDoc.isNull()) {
        qDebug() << "Failed to create QJsonDocument.";
        return;
    }

    QJsonObject jsonObj = jsonDoc.object();

    if (!jsonObj.contains("error")) {
        int err = ErrorCodes::ERR_JSON;
        qDebug() << "Auth Friend Failed, err is Json Parse Err" << err;
        return;
    }

    int err = jsonObj["error"].toInt();
	emit sig_user_message_business_rsp(jsonObj);
    if (err != ErrorCodes::SUCCESS) {
        qDebug() << "Auth Friend Failed, err is " << err;
		return;
	}
	const qint64 handled_message_id = jsonInt64(jsonObj["related_message_id"]);
	const auto handled_status = static_cast<FriendRequestStatus>(
		jsonObj["business_status"].toInt());
	UserMgr::GetInstance()->UpdateApplyStatus(handled_message_id, handled_status);
	emit sig_friend_request_handled(
		handled_message_id, static_cast<int>(handled_status));
	if (jsonObj["msg_type"].toInt() == static_cast<int>(ChatMsgType::FRIEND_ACCEPT)) {
		const QJsonObject peer = jsonObj["peer_profile"].toObject();
		auto rsp = std::make_shared<AuthRsp>(
			jsonObj["touid"].toInt(), peer["name"].toString(), peer["nick"].toString(),
			peer["icon"].toString(), peer["sex"].toInt());
		rsp->_thread_id = jsonInt64(jsonObj["thread_id"]);
		emit sig_auth_rsp(rsp);
	}
	return;
}

void TcpMgr::handleTextChatMsgRsp(ReqId id, int len, QByteArray data)
{
    Q_UNUSED(len);
    qDebug() << "handle id is " << id << " data is " << data;
    QJsonDocument jsonDoc = QJsonDocument::fromJson(data);

    if (jsonDoc.isNull()) {
        qDebug() << "Failed to create QJsonDocument.";
        return;
    }

    QJsonObject jsonObj = jsonDoc.object();

    if (!jsonObj.contains("error")) {
        qDebug() << "Chat Msg Rsp Failed, err is Json Parse Err";
        return;
    }

    int err = jsonObj["error"].toInt();
    if (err != ErrorCodes::SUCCESS) {
        //MESSAGE_CONFLICT/transient（2014/2016）原样转发，Dispatcher 判定
        qDebug() << "Chat Msg Rsp error, forward to dispatcher: " << err;
        emit sig_text_msg_rsp_forward(err, jsonObj.value("unique_id").toString(), 0, QString());
        return;
    }

    qDebug() << "Receive Text Chat Rsp Success " ;
    //message_id 十进制字符串解析转 qint64
    qint64 msg_id = jsonInt64(jsonObj["message_id"]);
    QString unique_id = jsonObj["unique_id"].toString();
    QString chat_time = jsonObj["chat_time"].toString();
    emit sig_text_msg_rsp_forward(err, unique_id, msg_id, chat_time);
}

void TcpMgr::handleNotifyOfflineReq(ReqId id, int len, QByteArray data)
{
    Q_UNUSED(len);
    qDebug() << "handle id is " << id << " data is " << data;
    // 将QByteArray转换为QJsonDocument
    QJsonDocument jsonDoc = QJsonDocument::fromJson(data);

    // 检查转换是否成功
    if (jsonDoc.isNull()) {
        qDebug() << "Failed to create QJsonDocument.";
        return;
    }

    QJsonObject jsonObj = jsonDoc.object();

    if (!jsonObj.contains("error")) {
        qDebug() << "Notify Chat Msg Failed, err is Json Parse Err";
        return;
    }

    int err = jsonObj["error"].toInt();
    if (err != ErrorCodes::SUCCESS) {
        qDebug() << "Notify Chat Msg Failed, err is " << err;
        return;
    }

    auto uid = jsonObj["uid"].toInt();
    qDebug() << "Receive offline Notify Success, uid is " << uid ;
    //断开连接
    //并且发送通知到界面
    emit sig_notify_offline();

}

void TcpMgr::handleHeartbeatRsp(ReqId id, int len, QByteArray data)
{
    Q_UNUSED(len);
    qDebug() << "handle id is " << id << " data is " << data;
    // 将QByteArray转换为QJsonDocument
    QJsonDocument jsonDoc = QJsonDocument::fromJson(data);

    // 检查转换是否成功
    if (jsonDoc.isNull()) {
        qDebug() << "Failed to create QJsonDocument.";
        return;
    }

    QJsonObject jsonObj = jsonDoc.object();

    if (!jsonObj.contains("error")) {
        qDebug() << "Heart Beat Msg Failed, err is Json Parse Err";
        return;
    }

    int err = jsonObj["error"].toInt();
    if (err != ErrorCodes::SUCCESS) {
        qDebug() << "Heart Beat Msg Failed, err is " << err;
        return;
    }

    qDebug() << "Receive Heart Beat Msg Success" ;

}

void TcpMgr::handleLoadChatThreadRsp(ReqId id, int len, QByteArray data)
{
    Q_UNUSED(len);
    qDebug() << "handle id is " << id << " data is " << data;
    // 将QByteArray转换为QJsonDocument
    QJsonDocument jsonDoc = QJsonDocument::fromJson(data);

    // 检查转换是否成功
    if (jsonDoc.isNull()) {
        qDebug() << "Failed to create QJsonDocument.";
        return;
    }

    QJsonObject jsonObj = jsonDoc.object();

    if (!jsonObj.contains("error")) {
        int err = ErrorCodes::ERR_JSON;
        qDebug() << "chat thread json parse failed " << err;
        return;
    }

    int err = jsonObj["error"].toInt();
    if (err != ErrorCodes::SUCCESS) {
        qDebug() << "get chat thread rsp failed, error is " << err;
        return;
    }

    qDebug() << "Receive chat thread rsp Success";

    auto thread_array = jsonObj["threads"].toArray();
    std::vector<std::shared_ptr<ChatThreadInfo>> chat_threads;
    for (const QJsonValue& value : thread_array) {
        auto cti = std::make_shared<ChatThreadInfo>();
        //thread_id/last_msg_id 十进制字符串解析转 qint64
        cti->_thread_id = jsonInt64(value["thread_id"]);
        cti->_type = value["type"].toString();
        cti->_user1_id = value["user1_id"].toInt();
        cti->_user2_id = value["user2_id"].toInt();
        cti->_last_msg_id = jsonInt64(value["last_msg_id"]);
        chat_threads.push_back(cti);
    }

    bool load_more = jsonObj["load_more"].toBool();
    qint64 next_last_id = jsonInt64(jsonObj["next_last_id"]);
    //发送信号通知界面
    emit sig_load_chat_thread(load_more, next_last_id, chat_threads);
}

void TcpMgr::handleCreatePrivateChatRsp(ReqId id, int len, QByteArray data)
{
    Q_UNUSED(len);
    qDebug() << "handle id is " << id << " data is " << data;
    // 将QByteArray转换为QJsonDocument
    QJsonDocument jsonDoc = QJsonDocument::fromJson(data);

    // 检查转换是否成功
    if (jsonDoc.isNull()) {
        qDebug() << "Failed to create QJsonDocument.";
        return;
    }

    QJsonObject jsonObj = jsonDoc.object();

    if (!jsonObj.contains("error")) {
        int err = ErrorCodes::ERR_JSON;
        qDebug() << "parse create private chat json parse failed " << err;
        return;
    }

    int err = jsonObj["error"].toInt();
    if (err != ErrorCodes::SUCCESS) {
        qDebug() << "get create private chat failed, error is " << err;
        return;
    }

    qDebug() << "Receive create private chat rsp Success";

    int uid = jsonObj["uid"].toInt();
    int other_id = jsonObj["other_id"].toInt();
    //thread_id 十进制字符串解析转 qint64
    qint64 thread_id = jsonInt64(jsonObj["thread_id"]);

    //发送信号通知界面
    emit sig_create_private_chat(uid, other_id, thread_id);
}

void TcpMgr::handleLoadChatMsgRsp(ReqId id, int len, QByteArray data)
{
    Q_UNUSED(len);
    qDebug() << "handle id is " << id << " data is " << data;
    // 将QByteArray转换为QJsonDocument
    QJsonDocument jsonDoc = QJsonDocument::fromJson(data);

    // 检查转换是否成功
    if (jsonDoc.isNull()) {
        qDebug() << "Failed to create QJsonDocument.";
        return;
    }

    QJsonObject jsonObj = jsonDoc.object();

    if (!jsonObj.contains("error")) {
        int err = ErrorCodes::ERR_JSON;
        qDebug() << "parse load chat msg json parse failed " << err;
        return;
    }

    int err = jsonObj["error"].toInt();
    if (err != ErrorCodes::SUCCESS) {
        qDebug() << "get load chat msg failed, error is " << err;
        return;
    }

    qDebug() << "Receive load chat msg rsp Success";

    //thread_id/last_message_id/msg_id 十进制字符串解析转 qint64
    qint64 thread_id = jsonInt64(jsonObj["thread_id"]);
    qint64 last_msg_id = jsonInt64(jsonObj["last_message_id"]);
    bool load_more = jsonObj["load_more"].toBool();

    std::vector<std::shared_ptr<ChatDataBase>> chat_datas;
    for (const QJsonValue& data : jsonObj["chat_datas"].toArray()) {
        auto send_uid = data["sender"].toInt();
        auto msg_id = jsonInt64(data["msg_id"]);
        auto msg_thread_id = jsonInt64(data["thread_id"]);
        auto msg_content = data["msg_content"].toString();
        QString chat_time = data["chat_time"].toString();
        int status = data["status"].toInt();
        int msg_type = data["msg_type"].toInt();
        int recv_id = data["receiver"].toInt();
        QString msg_content_hash = data["content_hash"].toString();
		if (msg_type == int(ChatMsgType::TEXT) ||
			msg_type == int(ChatMsgType::FRIEND_ACCEPT)) {
            auto chat_data = std::make_shared<TextChatData>(msg_id, msg_thread_id, ChatFormType::PRIVATE,
				static_cast<ChatMsgType>(msg_type), msg_content, send_uid, status, chat_time);
                chat_datas.push_back(chat_data);
                continue;
        }

        if (msg_type == int(ChatMsgType::PIC) || msg_type == int(ChatMsgType::FILE)) {
            //资源消息：下载缓存按 message_id 隔离；发送方另有本地归档目录
            auto uid = UserMgr::GetInstance()->GetUid();
            QString storageDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
            QString cache_dir = storageDir + "/user/" + QString::number(uid)
                + "/cache/" + QString::number(msg_id);
            QString cache_path = cache_dir + "/" + msg_content;
            QString own_path = storageDir + "/user/" + QString::number(uid)
                + "/resources/" + QString::number(send_uid) + "/" + QString::number(msg_id);
            const bool is_pic = (msg_type == int(ChatMsgType::PIC));
            QString local_path;
            if (QFile::exists(cache_path)) {
                local_path = cache_path;
            } else if (QFile::exists(own_path)) {
                local_path = own_path; //发送方自己的归档
            }

            if (local_path.isEmpty()) {
                //本地无缓存：占位上屏；图片自动下载，文件等用户点击
                CreatePlaceholderResourceMsgL(cache_dir, msg_content, msg_id, msg_thread_id,
                    send_uid, recv_id, status, chat_time,
                    is_pic ? ChatMsgType::PIC : ChatMsgType::FILE, chat_datas);
                continue;
            }

            QFileInfo fileInfo(local_path);
            qint64 file_size = fileInfo.size();
            QPixmap pixmap = is_pic ? QPixmap(local_path) : QPixmap();
            if (is_pic && pixmap.isNull()) {
                CreatePlaceholderResourceMsgL(cache_dir, msg_content, msg_id, msg_thread_id,
                    send_uid, recv_id, status, chat_time, ChatMsgType::PIC, chat_datas);
                continue;
            }

            auto file_info = std::make_shared<MsgInfo>(
                is_pic ? MsgType::IMG_MSG : MsgType::FILE_MSG,
                local_path, pixmap, msg_content, file_size, msg_content_hash);
            file_info->_msg_id = msg_id;
            file_info->_sender = send_uid;
            file_info->_receiver = recv_id;
            file_info->_thread_id = msg_thread_id;
            file_info->_transfer_type = TransferType::Download;
            file_info->_transfer_state = TransferState::Completed;
            file_info->_local_download_path = local_path;
            auto chat_data = std::make_shared<ImgChatData>(file_info, "", msg_thread_id,
                ChatFormType::PRIVATE, is_pic ? ChatMsgType::PIC : ChatMsgType::FILE,
                send_uid, status, chat_time);
            chat_datas.push_back(chat_data);
            continue;
        }
    }

    //发送信号通知界面
    emit sig_load_chat_msg(thread_id, last_msg_id, load_more, chat_datas);
}

void TcpMgr::handleCreateResourceMsgRsp(ReqId id, int len, QByteArray data)
{
    Q_UNUSED(len);
    qDebug() << "handle id is " << id << " data is " << data;
    QJsonDocument jsonDoc = QJsonDocument::fromJson(data);

    if (jsonDoc.isNull()) {
        qDebug() << "Failed to create QJsonDocument.";
        return;
    }

    QJsonObject jsonObj = jsonDoc.object();

    if (!jsonObj.contains("error")) {
        qDebug() << "parse create resource msg json parse failed";
        return;
    }

    int err = jsonObj["error"].toInt();
    if (err != ErrorCodes::SUCCESS) {
        //MESSAGE_CONFLICT/RESOURCE_*/transient（2014/2016）原样转发，Dispatcher 判定
        qDebug() << "create resource msg rsp error, forward to dispatcher: " << err;
        emit sig_resource_msg_meta_rsp_forward(err, jsonObj["unique_id"].toString(),
            QString(), 0, 0, 0, 0);
        return;
    }

    qDebug() << "Receive create resource msg rsp Success";

    //message_id/thread_id 十进制字符串解析转 qint64
    QString unique_id = jsonObj["unique_id"].toString();
    QString file_name = jsonObj["file_name"].toString();
    qint64 msg_id = jsonInt64(jsonObj["message_id"]);
    qint64 thread_id = jsonInt64(jsonObj["thread_id"]);
    int sender = jsonObj["fromuid"].toInt();
    int receiver = jsonObj["touid"].toInt();
    emit sig_resource_msg_meta_rsp_forward(err, unique_id, file_name,
        msg_id, thread_id, sender, receiver);
}

void TcpMgr::handleUserMessageNotify(ReqId id, int len, QByteArray data)
{
	Q_UNUSED(id);
	Q_UNUSED(len);
	const QJsonDocument jsonDoc = QJsonDocument::fromJson(data);
	if (!jsonDoc.isObject()) return;
	const QJsonObject envelope = jsonDoc.object();
	if (envelope["error"].toInt(ErrorCodes::ERR_JSON) != ErrorCodes::SUCCESS) return;
	emit sig_user_message_notify(envelope);
}

void TcpMgr::handleSyncMessageRsp(ReqId id, int len, QByteArray data)
{
    Q_UNUSED(len);
    QJsonDocument jsonDoc = QJsonDocument::fromJson(data);
    if (jsonDoc.isNull() || !jsonDoc.isObject()) {
        return;
    }
    emit sig_sync_message_rsp(jsonDoc.object());
}

void TcpMgr::CreatePlaceholderResourceMsgL(QString cache_dir, QString msg_content,
    qint64 msg_id, qint64 thread_id, int send_uid, int recv_id, int status, QString chat_time,
    ChatMsgType msg_type, std::vector<std::shared_ptr<ChatDataBase>> &chat_datas) {
    //本地无缓存：占位上屏（图片自动下载，文件等用户点击），传输上下文按 message_id 建立
    const bool is_pic = (msg_type == ChatMsgType::PIC);
    auto file_info = std::make_shared<MsgInfo>(
        is_pic ? MsgType::IMG_MSG : MsgType::FILE_MSG,
        cache_dir, CreateLoadingPlaceholder(200, 200), msg_content, 0, "");
    file_info->_msg_id = msg_id;
    file_info->_sender = send_uid;
    file_info->_receiver = recv_id;
    file_info->_thread_id = thread_id;
    file_info->_transfer_type = TransferType::Download;
    file_info->_transfer_state = is_pic ? TransferState::Downloading : TransferState::None;
    file_info->_rsp_size = file_info->_current_size;
    //放入chat_datas列表
    auto chat_data = std::make_shared<ImgChatData>(file_info, "", thread_id, ChatFormType::PRIVATE,
        msg_type, send_uid, status, chat_time);
    chat_datas.push_back(chat_data);
    //加入下载列表；图片自动下载（1511 元数据先行，FileTcpMgr 内部投递）
    UserMgr::GetInstance()->AddTransFile(msg_content, file_info);
    if (is_pic) {
        FileTcpMgr::GetInstance()->StartResourceDownload(file_info);
    }
}

void TcpMgr::handleMsg(ReqId id, int len, QByteArray data)
{
   auto find_iter =  _handlers.find(id);
   if(find_iter == _handlers.end()){
        qDebug()<< "not found id ["<< id << "] to handle";
        return ;
   }

   (this->*find_iter.value())(id, len, data);
}

void TcpMgr::finishReconnectFailure()
{
    if (!_reconnecting) {
        return;
    }

    _reconnecting = false;
    _manual_close = true;
    _socket.abort();
    emit sig_reconnect_finished(false);
}

void TcpMgr::slot_tcp_close() {
    _manual_close = true;
    _reconnecting = false;
    _socket.close();
}

void TcpMgr::slot_tcp_connect(std::shared_ptr<ServerInfo> si)
{
    qDebug()<< "receive tcp connect signal";
    _manual_close = false;
    _reconnecting = false;
    _disconnect_notified = false;
    // 3.2 保存 ServerInfo，Chat 认证成功后传给 FileTcpMgr 连接 Resource
    _server_info = si;
    // 尝试连接到服务器
    qDebug() << "Connecting to chat server...";
    _host = si->_chat_host;
    _port = static_cast<uint16_t>(si->_chat_port.toUInt());
    _socket.connectToHost(_host, _port);
}

void TcpMgr::slot_reconnect_chat(QString host, quint16 port)
{
    if (!_server_info || host.isEmpty() || port == 0 ||
        _socket.state() != QAbstractSocket::UnconnectedState) {
        emit sig_reconnect_finished(false);
        return;
    }

    _server_info->_chat_host = host;
    _server_info->_chat_port = QString::number(port);
    _host = host;
    _port = port;

    _buffer.clear();
    _b_recv_pending = false;
    _message_type = 0;
    _message_len = 0;
    _send_queue.clear();
    _current_block.clear();
    _bytes_sent = 0;
    _pending = false;

    _manual_close = false;
    _disconnect_notified = false;
    _reconnecting = true;
    qDebug() << "Reconnecting to chat server" << host << port;
    _socket.connectToHost(_host, _port);
}

void TcpMgr::slot_send_data(ReqId reqId, QByteArray dataBytes)
{
    uint16_t id = reqId;

    // 计算长度（使用网络字节序转换）
    quint16 len = static_cast<quint16>(dataBytes.length());

    // 创建一个QByteArray用于存储要发送的所有数据
    QByteArray block;
    QDataStream out(&block, QIODevice::WriteOnly);

    // 设置数据流使用网络字节序
    out.setByteOrder(QDataStream::BigEndian);

    // 写入ID和长度
    out << id << len;

    // 添加字符串数据
    block.append(dataBytes);

    //判断是否正在发送
    if (_pending) {
        //放入队列直接返回，因为目前有数据正在发送
        _send_queue.enqueue(block);
        return;
    }

    // 没有正在发送，把这包设为“当前块”，重置计数，并写出去
    _current_block = block;        // ← 保存当前正在发送的 block
    _bytes_sent = 0;            // ← 归零
    _pending = true;         // ← 标记正在发送

    _socket.write(_current_block);
}

TcpThread::TcpThread()
{
    _tcp_thread = new QThread();
    TcpMgr::GetInstance()->moveToThread(_tcp_thread);
    //OutboxDispatcher/ChatSyncManager 驻留 TCP 线程（独立 QObject）
    OutboxDispatcher::GetInstance()->moveToThread(_tcp_thread);
    ChatSyncManager::GetInstance()->moveToThread(_tcp_thread);
    QObject::connect(_tcp_thread, &QThread::finished, _tcp_thread, &QObject::deleteLater);

    _tcp_thread->start();
}

TcpThread::~TcpThread()
{
    _tcp_thread->quit();
    _tcp_thread->wait();
}
