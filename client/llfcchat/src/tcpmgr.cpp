#include "tcpmgr.h"
#include <QAbstractSocket>
#include <QCoreApplication>
#include "usermgr.h"
#include <QTimer>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSettings>
#include <QMessageBox>
#include <QJsonArray>
#include <filetcpmgr.h>
#include <QStandardPaths>

TcpMgr::TcpMgr():_host(""),_port(0),_b_recv_pending(false),_message_id(0),_message_len(0),_bytes_sent(0),_pending(false),
    _retry_timer(nullptr),_delivery_uid(0),_retry_initial_ms(2000),_retry_max_ms(30000),
    _ack_retry_initial_ms(2000),_offline_pull_timer(nullptr),_offline_pull_interval_ms(10000),_offline_pull_batch(100)
{
    registerMetaType();
    QObject::connect(&_socket, &QTcpSocket::connected, this, [&]() {
           qDebug() << "Connected to server!";
           // 连接建立后发送消息
            emit sig_con_success(true);
       });

       QObject::connect(&_socket, &QTcpSocket::readyRead, this, [&]() {
           // 当有数据可读时，读取所有数据
           // 读取所有数据并追加到缓冲区
           _buffer.append(_socket.readAll());

           forever {
                //先解析头部
               if(!_b_recv_pending){
                   // 检查缓冲区中的数据是否足够解析出一个消息头（消息ID + 消息长度）
                   if (_buffer.size() < static_cast<int>(sizeof(quint16) * 2)) {
                       return; // 数据不够，等待更多数据
                   }

                   // ✅ 每次都重新创建stream
                   QDataStream stream(_buffer);
                   stream.setVersion(QDataStream::Qt_5_0);
                   stream >> _message_id >> _message_len;
                   _buffer.remove(0, sizeof(quint16) * 2);  // 使用remove代替mid赋值
                   qDebug() << "Message ID:" << _message_id << ", Length:" << _message_len;

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
               handleMsg(ReqId(_message_id),_message_len, messageBody);
           }

       });

       //5.15 之后版本
//       QObject::connect(&_socket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::errorOccurred), [&](QAbstractSocket::SocketError socketError) {
//           Q_UNUSED(socketError)
//           qDebug() << "Error:" << _socket.errorString();
//       });

       // 处理错误（适用于Qt 5.15之前的版本）
        QObject::connect(&_socket, static_cast<void (QTcpSocket::*)(QTcpSocket::SocketError)>(&QTcpSocket::error),
                            this,
                            [&](QTcpSocket::SocketError socketError) {
               qDebug() << "Error:" << _socket.errorString() ;
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
            //暂停重传定时器（pending 保留在内存，重连后恢复）
            if (_retry_timer) {
                _retry_timer->stop();
            }
            //§6.6 断线停止离线 pull
            if (_offline_pull_timer) {
                _offline_pull_timer->stop();
            }
            //并且发送通知到界面
            emit sig_connection_closed();
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
        //可靠重传信号连接（公有 API → TCP 线程 slot）
        connect(this, &TcpMgr::sig_send_reliable_chat, this, &TcpMgr::slot_send_reliable_chat);
        //§6.2：StartPendingReplay 公有 API → TCP 线程 slot（GUI thread models 建好后执行）
        connect(this, &TcpMgr::sig_start_pending_replay, this, &TcpMgr::slot_start_pending_replay);
        //§6.2 纠错：GUI 重建回执 → TCP 线程清理失效项 + 重发 + 定时器
        connect(this, &TcpMgr::sig_replay_result, this, &TcpMgr::slot_replay_done);
        //§6.5：ChatDialog 插入/duplicate 后 emit → queued 回 TCP 线程触发 1049
        connect(this, &TcpMgr::sig_chat_msg_processed, this, &TcpMgr::slot_msg_processed);
        //§6.6：StartOfflinePull 公有 API → TCP 线程 slot
        connect(this, &TcpMgr::sig_start_offline_pull, this, &TcpMgr::slot_start_offline_pull);
        //250ms 重传扫描定时器，parent 到 this，随 moveToThread 迁移到 TCP 线程
        _retry_timer = new QTimer(this);
        _retry_timer->setInterval(250);
        connect(_retry_timer, &QTimer::timeout, this, &TcpMgr::slot_retry_timeout);
        //§6.6 离线 pull 定时器，parent 到 this，随 moveToThread 迁移到 TCP 线程
        _offline_pull_timer = new QTimer(this);
        _offline_pull_timer->setInterval(_offline_pull_interval_ms);
        connect(_offline_pull_timer, &QTimer::timeout, this, &TcpMgr::slot_offline_pull_timeout);
        //读取 [Delivery] 配置
        loadDeliveryConfig();
        //3.2 一次性净化旧 QSettings 离线队列中的 token 字段（迁移到 auth_payload_version=2）
        sanitizeLegacyDeliverySettings();
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

    //§6.2 纠错：跨线程 replay DTO 元类型注册
    qRegisterMetaType<TextReplayDTO>("TextReplayDTO");
    qRegisterMetaType<ImageReplayDTO>("ImageReplayDTO");
    qRegisterMetaType<std::vector<TextReplayDTO>>("std::vector<TextReplayDTO>");
    qRegisterMetaType<std::vector<ImageReplayDTO>>("std::vector<ImageReplayDTO>");
}

void TcpMgr::CloseConnection(){
    emit sig_close();
}

void TcpMgr::SendData(ReqId reqId, QByteArray data)
{
    emit sig_send_data(reqId, data);
}

void TcpMgr::SendReliableChat(ReqId id, QByteArray payload, const QStringList& unique_ids)
{
    //公有 API：只发 signal，实际 pending/发送在 TCP 线程的 slot 中执行
    QStringList ids = unique_ids;
    emit sig_send_reliable_chat(id, payload, ids);
}

void TcpMgr::StartPendingReplay()
{
    //§6.2：公有 API 只 emit queued signal，实际 rebuild+重发在 TCP 线程 slot 执行
    //调用点：ChatDialog::slot_load_chat_thread 最后一页 load_more=false（GUI thread models 已建好）
    emit sig_start_pending_replay();
}

void TcpMgr::StartOfflinePull()
{
    //§6.6：公有 API 只 emit queued signal，TCP 线程 slot 启动离线 pull 循环
    //调用点：ChatDialog::slot_load_chat_thread 最后一页 load_more=false 后
    emit sig_start_offline_pull();
}



TcpMgr::~TcpMgr(){

}

void TcpMgr::initHandlers()
{
    //auto self = shared_from_this();
    _handlers.insert(ID_CHAT_LOGIN_RSP, [this](ReqId id, int len, QByteArray data){
        Q_UNUSED(len);
        qDebug()<< "handle id is "<< id ;
        // 将QByteArray转换为QJsonDocument
        QJsonDocument jsonDoc = QJsonDocument::fromJson(data);

        // 检查转换是否成功
        if(jsonDoc.isNull()){
           qDebug() << "Failed to create QJsonDocument.";
           return;
        }

        QJsonObject jsonObj = jsonDoc.object();
        qDebug()<< "data jsonobj is " << jsonObj ;

        if(!jsonObj.contains("error")){
            int err = ErrorCodes::ERR_JSON;
            qDebug() << "Login Failed, err is Json Parse Err" << err ;
            emit sig_login_failed(err);
            return;
        }

        int err = jsonObj["error"].toInt();
        if(err != ErrorCodes::SUCCESS){
            qDebug() << "Login Failed, err is " << err ;
            emit sig_login_failed(err);
            return;
        }
        
        auto uid = jsonObj["uid"].toInt();
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

        //恢复持久 pending 请求（仅同 uid，不同账号绝不互载）
        restorePendingRequests(uid);

        //3.2 Chat 认证成功后触发 FileTcpMgr 连接 Resource（UI 切换延迟到 Resource 鉴权成功）
        emit sig_connect_resource(_server_info);
    });


	_handlers.insert(ID_SEARCH_USER_RSP, [this](ReqId id, int len, QByteArray data) {
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
		});

	_handlers.insert(ID_NOTIFY_ADD_FRIEND_REQ, [this](ReqId id, int len, QByteArray data) {
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

         int from_uid = jsonObj["applyuid"].toInt();
         QString name = jsonObj["name"].toString();
         QString desc = jsonObj["desc"].toString();
         QString icon = jsonObj["icon"].toString();
         QString nick = jsonObj["nick"].toString();
         int sex = jsonObj["sex"].toInt();

        auto apply_info = std::make_shared<AddFriendApply>(
                    from_uid, name, desc,
                      icon, nick, sex);

		emit sig_friend_apply(apply_info);
		});

    _handlers.insert(ID_NOTIFY_AUTH_FRIEND_REQ, [this](ReqId id, int len, QByteArray data) {
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
            qDebug() << "Auth Friend Failed, err is " << err;
            return;
        }

        int err = jsonObj["error"].toInt();
        if (err != ErrorCodes::SUCCESS) {
            qDebug() << "Auth Friend Failed, err is " << err;
            return;
        }

        int from_uid = jsonObj["fromuid"].toInt();
        QString name = jsonObj["name"].toString();
        QString nick = jsonObj["nick"].toString();
        QString icon = jsonObj["icon"].toString();
        int sex = jsonObj["sex"].toInt();

        std::vector<std::shared_ptr<TextChatData>> chat_datas;
        for (const QJsonValue& data : jsonObj["chat_datas"].toArray()) {
            auto send_uid = data["sender"].toInt();
            auto msg_id = data["msg_id"].toInt();
            auto thread_id = data["thread_id"].toInt();
            auto unique_id = data["unique_id"].toInt();
            auto msg_content = data["msg_content"].toString();
            QString chat_time = data["chat_time"].toString();
            auto status = data["status"].toInt();
            auto chat_data = std::make_shared<TextChatData>(msg_id, thread_id, ChatFormType::PRIVATE,
                ChatMsgType::TEXT, msg_content, send_uid, status, chat_time);
            chat_datas.push_back(chat_data);
        }

        auto auth_info = std::make_shared<AuthInfo>(from_uid,name,
                                                    nick, icon, sex);

        auth_info->SetChatDatas(chat_datas);

        emit sig_add_auth_friend(auth_info);
        });

    _handlers.insert(ID_ADD_FRIEND_RSP, [this](ReqId id, int len, QByteArray data) {
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
        if (err != ErrorCodes::SUCCESS) {
            qDebug() << "Add Friend Failed, err is " << err;
            return;
        }

         qDebug() << "Add Friend Success " ;
      });


    _handlers.insert(ID_AUTH_FRIEND_RSP, [this](ReqId id, int len, QByteArray data) {
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
        if (err != ErrorCodes::SUCCESS) {
            qDebug() << "Auth Friend Failed, err is " << err;
            return;
        }

        auto name = jsonObj["name"].toString();
        auto nick = jsonObj["nick"].toString();
        auto icon = jsonObj["icon"].toString();
        auto sex = jsonObj["sex"].toInt();
        auto uid = jsonObj["uid"].toInt();
        
        std::vector<std::shared_ptr<TextChatData>> chat_datas;
        for (const QJsonValue& data : jsonObj["chat_datas"].toArray()) {
            auto send_uid = data["sender"].toInt();
            auto msg_id = data["msg_id"].toInt();
            auto thread_id = data["thread_id"].toInt();
            auto unique_id = data["unique_id"].toInt();
            auto msg_content = data["msg_content"].toString();
            auto status = data["status"].toInt();
            auto chat_data = std::make_shared<TextChatData>(msg_id, thread_id, ChatFormType::PRIVATE,
                ChatMsgType::TEXT, msg_content, send_uid, status);
            chat_datas.push_back(chat_data);
        }

        auto rsp = std::make_shared<AuthRsp>(uid, name, nick, icon, sex);
        rsp->SetChatDatas(chat_datas);
        emit sig_auth_rsp(rsp);

        qDebug() << "Auth Friend Success " ;
      });


    _handlers.insert(ID_TEXT_CHAT_MSG_RSP, [this](ReqId id, int len, QByteArray data) {
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
            qDebug() << "Chat Msg Rsp Failed, err is Json Parse Err" << err;
            return;
        }

        int err = jsonObj["error"].toInt();
        if (err == ErrorCodes::MESSAGE_CONFLICT) {
            //永久冲突：按 conflict_unique_ids 停止重传并标 SEND_FAILED
            qDebug() << "Text Chat Conflict (1017), stopping retry for conflicted items";
            auto thread_id = jsonObj.value("thread_id").toInt();
            auto sender = jsonObj.value("fromuid").toInt();
            QStringList conflict_ids;
            if (jsonObj.contains("conflict_unique_ids")) {
                for (const auto& v : jsonObj["conflict_unique_ids"].toArray()) {
                    conflict_ids.append(v.toString());
                }
            }
            handleTextConflict(thread_id, sender, conflict_ids);
            return;
        }
        if (err != ErrorCodes::SUCCESS) {
            //transient（1014/1016）或未知错误：不清 pending，定时器继续重传
            qDebug() << "Chat Msg Rsp transient error, will retry: " << err;
            return;
        }

        qDebug() << "Receive Text Chat Rsp Success " ;
        //收到消息后转发给页面
        auto thread_id = jsonObj["thread_id"].toInt();
        auto sender = jsonObj["fromuid"].toInt();


        std::vector<std::shared_ptr<TextChatData>> chat_datas;
        for (const QJsonValue& data : jsonObj["chat_datas"].toArray()) {      
            auto msg_id = data["message_id"].toInt();
            auto unique_id = data["unique_id"].toString();
            auto msg_content = data["content"].toString();
            QString chat_time = data["chat_time"].toString();
            int status = data["status"].toInt();
            auto chat_data = std::make_shared<TextChatData>(msg_id,unique_id, thread_id, ChatFormType::PRIVATE,
                ChatMsgType::TEXT, msg_content, sender, status, chat_time);
            chat_datas.push_back(chat_data);
            //清理已确认的 unique_id（批量未覆盖部分保留继续重传）
            removePendingByUniqueId(unique_id);
        }
        //pending 变更后持久化
        persistPendingRequests();

        //发送信号通知界面
        emit sig_chat_msg_rsp(thread_id, chat_datas);

      });

    _handlers.insert(ID_NOTIFY_TEXT_CHAT_MSG_REQ, [this](ReqId id, int len, QByteArray data) {
        Q_UNUSED(len);
        qDebug() << "handle id is " << id << " data is " << data;
        QJsonDocument jsonDoc = QJsonDocument::fromJson(data);

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

        qDebug() << "Receive Text Chat Notify Success " ;

        //§6.4 统一 envelope：顶层 thread_id/fromuid/touid，每个 chat_datas 元素含 per-message 字段
        auto top_thread_id = jsonObj["thread_id"].toInt();
        auto top_fromuid = jsonObj["fromuid"].toInt();
        auto top_touid = jsonObj.value("touid").toInt();

        for (const QJsonValue& elem : jsonObj["chat_datas"].toArray()) {
            int msg_id = elem["message_id"].toInt();
            QString unique_id = elem["unique_id"].toString();
            QString msg_content = elem["content"].toString();
            QString chat_time = elem["chat_time"].toString();
            int status = elem["status"].toInt();
            int msg_type = elem["msg_type"].toInt(static_cast<int>(ChatMsgType::TEXT));
            qint64 content_size = elem["content_size"].toString().toLongLong();
            //元素可自带 thread_id/fromuid（统一 envelope），回退到顶层
            int el_thread = elem["thread_id"].toInt(top_thread_id);
            int el_from = elem["fromuid"].toInt(top_fromuid);
            int el_to = elem["touid"].toInt(top_touid);
            dispatchIncomingMessage(msg_id, unique_id, el_thread, el_from, el_to,
                msg_type, msg_content, content_size, chat_time, status);
        }
      });

    _handlers.insert(ID_NOTIFY_OFF_LINE_REQ,[this](ReqId id, int len, QByteArray data){
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
            qDebug() << "Notify Chat Msg Failed, err is Json Parse Err" << err;
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

    });

    _handlers.insert(ID_HEARTBEAT_RSP,[this](ReqId id, int len, QByteArray data){
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
            qDebug() << "Heart Beat Msg Failed, err is Json Parse Err" << err;
            return;
        }

        int err = jsonObj["error"].toInt();
        if (err != ErrorCodes::SUCCESS) {
            qDebug() << "Heart Beat Msg Failed, err is " << err;
            return;
        }

        qDebug() << "Receive Heart Beat Msg Success" ;

    });


    _handlers.insert(ID_LOAD_CHAT_THREAD_RSP, [this](ReqId id, int len, QByteArray data) {
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
            cti->_thread_id = value["thread_id"].toInt();
            cti->_type = value["type"].toString();
            cti->_user1_id = value["user1_id"].toInt();
            cti->_user2_id = value["user2_id"].toInt();
            chat_threads.push_back(cti);
        }

        bool load_more = jsonObj["load_more"].toBool();
        int next_last_id = jsonObj["next_last_id"].toInt();
        //发送信号通知界面
        emit sig_load_chat_thread(load_more, next_last_id, chat_threads);
    });


    _handlers.insert(ID_CREATE_PRIVATE_CHAT_RSP, [this](ReqId id, int len, QByteArray data) {
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
        int thread_id = jsonObj["thread_id"].toInt();

        //发送信号通知界面
        emit sig_create_private_chat(uid, other_id, thread_id);
        });



    _handlers.insert(ID_LOAD_CHAT_MSG_RSP, [this](ReqId id, int len, QByteArray data) {
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

        int thread_id = jsonObj["thread_id"].toInt();
        int last_msg_id = jsonObj["last_message_id"].toInt();
        bool load_more = jsonObj["load_more"].toBool();

        std::vector<std::shared_ptr<ChatDataBase>> chat_datas;
        for (const QJsonValue& data : jsonObj["chat_datas"].toArray()) {
            auto send_uid = data["sender"].toInt();
            auto msg_id = data["msg_id"].toInt();
            auto thread_id = data["thread_id"].toInt();
            auto unique_id = data["unique_id"].toInt();
            auto msg_content = data["msg_content"].toString();
            QString chat_time = data["chat_time"].toString();
            int status = data["status"].toInt();
            int msg_type = data["msg_type"].toInt();
            int recv_id = data["receiver"].toInt();
            if (msg_type == int(ChatMsgType::TEXT)) {
                auto chat_data = std::make_shared<TextChatData>(msg_id, thread_id, ChatFormType::PRIVATE,
                    ChatMsgType::TEXT, msg_content, send_uid, status, chat_time);
                    chat_datas.push_back(chat_data);
                    continue;
            }
            
            if (msg_type == int(ChatMsgType::PIC)) {
                auto uid = UserMgr::GetInstance()->GetUid();
                QString storageDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
                QString img_path_str = storageDir + "/user/" + QString::number(uid) + "/chatimg/" + QString::number(send_uid);
                QString img_path = img_path_str + "/" + msg_content;
                //文件不存在，则创建空白图片占位，同时组织数据准备发送
                if (QFile::exists(img_path) == false) {
                    
                    CreatePlaceholderImgMsgL(img_path_str, msg_content,
                        msg_id, thread_id, send_uid, recv_id, status, chat_time,
                        chat_datas);
                    continue;
                }
                //如果文件存在
                //如果文件存在则直接构建MsgInfo
                // 获取文件大小
                QFileInfo fileInfo(img_path);
                qint64 file_size = fileInfo.size();
                //从文件路径加载QPixmap
                QPixmap pixmap(img_path);
                //如果图片加载失败，也是创建占位符，然后组织发送
                if (pixmap.isNull()) {
                    CreatePlaceholderImgMsgL(img_path_str, msg_content,
                        msg_id, thread_id, send_uid, recv_id, status, chat_time,
                        chat_datas);
                        continue;
                }

                //说明图片加载正确，构建真实图片
                auto  file_info = std::make_shared<MsgInfo>(MsgType::IMG_MSG, img_path_str,
                    pixmap, msg_content, file_size, "");
                file_info->_msg_id = msg_id;
                file_info->_sender = send_uid;
                file_info->_receiver = recv_id;
                file_info->_thread_id = thread_id;
                //设置文件传输的类型
                file_info->_transfer_type = TransferType::Download;
                //设置文件传输状态
                file_info->_transfer_state = TransferState::None;
                //放入chat_datas列表
                auto chat_data = std::make_shared<ImgChatData>(file_info,"", thread_id, ChatFormType::PRIVATE,
                    ChatMsgType::PIC, send_uid, status, chat_time);
                chat_datas.push_back(chat_data);
                continue;
            }          
        }

        //发送信号通知界面
        emit sig_load_chat_msg(thread_id, last_msg_id, load_more, chat_datas);
        });

    _handlers.insert(ID_IMG_CHAT_MSG_RSP, [this](ReqId id, int len, QByteArray data) {
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
        if (err == ErrorCodes::MESSAGE_CONFLICT) {
            //永久冲突：停止该 unique_id 重传并标 SEND_FAILED
            qDebug() << "Img Chat Conflict (1017), stopping retry";
            QString conflict_id;
            if (jsonObj.contains("conflict_unique_ids")) {
                auto arr = jsonObj["conflict_unique_ids"].toArray();
                if (!arr.isEmpty()) {
                    conflict_id = arr.at(0).toString();
                }
            }
            if (conflict_id.isEmpty() && jsonObj.contains("unique_id")) {
                conflict_id = jsonObj["unique_id"].toString();
            }
            handleImageConflict(conflict_id);
            return;
        }
        if (err != ErrorCodes::SUCCESS) {
            //transient（1014/1016）：不清 pending，定时器继续重传
            qDebug() << "get create private chat transient error, will retry: " << err;
            return;
        }

        qDebug() << "Receive create private chat rsp Success";

        //收到消息后转发给页面
        auto thread_id = jsonObj["thread_id"].toInt();
        auto unique_id = jsonObj["unique_id"].toString();
        auto unique_name = jsonObj["unique_name"].toString();
        
        auto sender = jsonObj["fromuid"].toInt();
        auto msg_id = jsonObj["message_id"].toInt();
        QString chat_time = jsonObj["chat_time"].toString();
        int status = jsonObj["status"].toInt();
        auto text_or_url = jsonObj["text_or_url"].toString();
        auto receiver = jsonObj["touid"].toInt();

        auto file_info = UserMgr::GetInstance()->GetTransFileByName(unique_name);
        //如果未找到文件对应的信息则返回
        if (!file_info) {
            return;
        }
        //设置消息id和会话id
        file_info->_msg_id = msg_id;
        file_info->_thread_id = thread_id;
        //设置发送者和接收者
        file_info->_sender = sender;
        file_info->_receiver = receiver;
        //设置文件传输的类型
        file_info->_transfer_type = TransferType::Upload;
        //设置文件传输状态
        file_info->_transfer_state = TransferState::Uploading;
 
        auto chat_data = std::make_shared<ImgChatData>(file_info, unique_id, thread_id, ChatFormType::PRIVATE,
            ChatMsgType::PIC, sender, status, chat_time);

        //更新msg_id,因为最开始构造的chat_dat中ImgChatData的msg_id为空
        chat_data->SetMsgId(msg_id);

        //发送信号通知界面
        emit sig_chat_img_rsp(thread_id, chat_data);

        //清理已确认的 unique_id（服务端已持久化）
        removePendingByUniqueId(unique_id);
        persistPendingRequests();

        //管理消息，添加序列号到正在发送集合
        file_info->_flighting_seqs.insert(file_info->_seq);
        
        //发送消息
        QFile file(file_info->_text_or_url);
        if (!file.open(QIODevice::ReadOnly)) {
            qWarning() << "Could not open file:" << file.errorString();
            return;
        }
        
        file.seek(file_info->_current_size);
        auto buffer = file.read(MAX_FILE_LEN);
        qDebug() << "buffer is " << buffer;
        //将文件内容转换为base64编码
        QString base64Data = buffer.toBase64();
        QJsonObject file_obj;
        file_obj["name"] = file_info->_unique_name;
        file_obj["unique_id"] = unique_id;
        file_obj["seq"] = file_info->_seq;
        file_info->_current_size = buffer.size() + (file_info->_seq - 1) * MAX_FILE_LEN;
        file_obj["trans_size"] = QString::number(file_info->_current_size);
        file_obj["total_size"] = QString::number(file_info->_total_size);
        file_obj["md5"] = file_info->_md5;
        //3.2 token/uid/sender 由 Resource 从 session 派生，不再发送
        file_obj["data"] = base64Data;
        file_obj["message_id"] = msg_id;
        file_obj["receiver"] = receiver;

        if (buffer.size() + (file_info->_seq - 1) * MAX_FILE_LEN >= file_info->_total_size) {
            file_obj["last"] = 1;
        }
        else {
            file_obj["last"] = 0;
        }

        //发送文件  todo 留作以后收到服务器返回消息后再发送
		QJsonDocument doc_file(file_obj);
		QByteArray fileData = doc_file.toJson(QJsonDocument::Compact);

        //发送消息给ResourceServer
        FileTcpMgr::GetInstance()->SendData(ReqId::ID_FILE_INFO_SYNC_REQ, fileData);

        });

    
     _handlers.insert(ID_NOTIFY_IMG_CHAT_MSG_REQ, [this](ReqId id, int len, QByteArray data) {
         Q_UNUSED(len);
         qDebug() << "handle id is " << id << " data is " << data;
         QJsonDocument jsonDoc = QJsonDocument::fromJson(data);

         if (jsonDoc.isNull()) {
             qDebug() << "Failed to create QJsonDocument.";
             return;
         }

         QJsonObject jsonObj = jsonDoc.object();
         qDebug() << "receive notify img chat msg req success" ;

         //§6.4 统一 envelope（计划6.4）：兼容旧 sender_id/receiver_id/img_name/total_size
         int message_id = jsonObj["message_id"].toInt();
         QString unique_id = jsonObj["unique_id"].toString();
         int thread_id = jsonObj["thread_id"].toInt();
         int fromuid = jsonObj["fromuid"].toInt(jsonObj["sender_id"].toInt());
         int touid = jsonObj["touid"].toInt(jsonObj["receiver_id"].toInt());
         int msg_type = jsonObj["msg_type"].toInt(static_cast<int>(ChatMsgType::PIC));
         QString content = jsonObj["content"].toString(jsonObj["img_name"].toString());
         qint64 content_size = jsonObj["content_size"].toString().toLongLong();
         if (content_size == 0) {
             content_size = jsonObj["total_size"].toString().toLongLong();
         }
         //服务端不返回 chat_time/status，客户端按现有下载状态构造
         dispatchIncomingMessage(message_id, unique_id, thread_id, fromuid, touid,
             msg_type, content, content_size, QString(), MsgStatus::READED);
     });

    //§6.5 ACK response：清理已确认 message_id，transient 错误保留继续重试
    _handlers.insert(ID_CHAT_DELIVERY_ACK_RSP, [this](ReqId id, int len, QByteArray data) {
        Q_UNUSED(len);
        QJsonDocument jsonDoc = QJsonDocument::fromJson(data);
        if (jsonDoc.isNull()) {
            return;
        }
        QJsonObject jsonObj = jsonDoc.object();
        int err = jsonObj["error"].toInt();
        if (err != ErrorCodes::SUCCESS) {
            qDebug() << "[ACK] transient rsp error" << err << "keeping pending";
            return;
        }
        for (const QJsonValue& v : jsonObj["message_ids"].toArray()) {
            _pending_ack.remove(v.toInt());
        }
        persistAckPending();
        qDebug() << "[ACK] confirmed, remaining pending_ack=" << _pending_ack.size();
    });

    //§6.6 离线 pull response：逐条走 dispatchIncomingMessage，has_more 连续取下一页
    _handlers.insert(ID_PULL_OFFLINE_MSG_RSP, [this](ReqId id, int len, QByteArray data) {
        Q_UNUSED(len);
        QJsonDocument jsonDoc = QJsonDocument::fromJson(data);
        if (jsonDoc.isNull()) {
            return;
        }
        QJsonObject jsonObj = jsonDoc.object();
        int err = jsonObj["error"].toInt();
        if (err != ErrorCodes::SUCCESS) {
            qDebug() << "[OfflinePull] error" << err << "will retry next interval";
            return;
        }
        int next_message_id = jsonObj["next_message_id"].toInt();
        bool has_more = jsonObj["has_more"].toBool();

        for (const QJsonValue& elem : jsonObj["messages"].toArray()) {
            int msg_id = elem["message_id"].toInt();
            QString unique_id = elem["unique_id"].toString();
            int el_thread = elem["thread_id"].toInt();
            int el_from = elem["fromuid"].toInt();
            int el_to = elem["touid"].toInt();
            int msg_type = elem["msg_type"].toInt(static_cast<int>(ChatMsgType::TEXT));
            QString content = elem["content"].toString();
            qint64 content_size = elem["content_size"].toString().toLongLong();
            QString chat_time = elem["chat_time"].toString();
            int status = elem["status"].toInt();
            dispatchIncomingMessage(msg_id, unique_id, el_thread, el_from, el_to,
                msg_type, content, content_size, chat_time, status);
        }

        //has_more：立即拉下一页（同一连接 FIFO 顺序）；下一轮 timer 重新从 0 开始
        if (has_more && _socket.state() == QAbstractSocket::ConnectedState && _delivery_uid != 0) {
            QJsonObject next_obj;
            next_obj["uid"] = _delivery_uid;
            next_obj["after_message_id"] = next_message_id;
            next_obj["limit"] = _offline_pull_batch;
            QJsonDocument ndoc(next_obj);
            slot_send_data(ID_PULL_OFFLINE_MSG_REQ, ndoc.toJson(QJsonDocument::Compact));
        }
    });
    
}

void TcpMgr::CreatePlaceholderImgMsgL(QString img_path_str, QString msg_content, 
    int msg_id, int thread_id, int send_uid, int recv_id, int status, QString chat_time,
    std::vector<std::shared_ptr<ChatDataBase>> &chat_datas) {
    //如果加载失败，则使用占位符使图片变为空白，并且md5为空
    auto  file_info = std::make_shared<MsgInfo>(MsgType::IMG_MSG, img_path_str,
        CreateLoadingPlaceholder(200, 200), msg_content, 0, "");
    file_info->_msg_id = msg_id;
    file_info->_sender = send_uid;
    file_info->_receiver = recv_id;
    file_info->_thread_id = thread_id;
    //设置文件传输的类型
    file_info->_transfer_type = TransferType::Download;
    //设置文件传输状态
    file_info->_transfer_state = TransferState::Downloading;
    file_info->_rsp_size = file_info->_current_size;
    //放入chat_datas列表
    auto chat_data = std::make_shared<ImgChatData>(file_info, "", thread_id, ChatFormType::PRIVATE,
        ChatMsgType::PIC, send_uid, status, chat_time);
    chat_datas.push_back(chat_data);
    //加入下载列表，并且发送下载请求
    UserMgr::GetInstance()->AddTransFile(msg_content, file_info);
 
    QJsonObject jsonObj_send;
    jsonObj_send["message_id"] = file_info->_msg_id;
    QJsonDocument doc(jsonObj_send);
    auto send_data = doc.toJson();
    // 从服务器获取文件大小，然后请求下载
    FileTcpMgr::GetInstance()->SendData(ID_IMG_CHAT_DOWN_INFO_SYNC_REQ, send_data);
}

void TcpMgr::handleMsg(ReqId id, int len, QByteArray data)
{
   auto find_iter =  _handlers.find(id);
   if(find_iter == _handlers.end()){
        qDebug()<< "not found id ["<< id << "] to handle";
        return ;
   }

   find_iter.value()(id,len,data);
}

void TcpMgr::slot_tcp_close() {
    _socket.close();
}

void TcpMgr::slot_tcp_connect(std::shared_ptr<ServerInfo> si)
{
    qDebug()<< "receive tcp connect signal";
    // 3.2 保存 ServerInfo，Chat 认证成功后传给 FileTcpMgr 连接 Resource
    _server_info = si;
    // 尝试连接到服务器
    qDebug() << "Connecting to chat server...";
    _host = si->_chat_host;
    _port = static_cast<uint16_t>(si->_chat_port.toUInt());
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

    qint64 written = _socket.write(_current_block);
   /* qDebug() << "tcp mgr send byte data is" << _current_block
        << ", write() returned" << written;*/
}

//—— 可靠重传实现（均在 TCP 线程执行）——

//§6.4 统一 envelope 分发：1019/1039/1052 共用。message_id 进入待 UI 确认集合。
//文本走 sig_text_chat_msg，图片复用 1039 下载流程走 sig_img_chat_msg。
void TcpMgr::dispatchIncomingMessage(int message_id, const QString& unique_id,
    int thread_id, int fromuid, int touid, int msg_type,
    const QString& content, qint64 content_size,
    const QString& chat_time, int status)
{
    //§6.5：message_id 进入待 UI 完成集合（UI 插入/去重后才 ACK）
    _pending_ui_ack.insert(message_id);

    if (msg_type == static_cast<int>(ChatMsgType::PIC)) {
        //图片消息：复用 1039 下载流程
        auto uid = UserMgr::GetInstance()->GetUid();
        QString storageDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QString img_path_str = storageDir + "/user/" + QString::number(uid)
            + "/chatimg/" + QString::number(fromuid);
        QString img_name = content;
        qint64 total_size = content_size;

        auto file_info = UserMgr::GetInstance()->GetTransFileByName(img_name);
        if (!file_info) {
            file_info = std::make_shared<MsgInfo>(MsgType::IMG_MSG, img_path_str,
                CreateLoadingPlaceholder(200, 200), img_name, total_size, "");
            UserMgr::GetInstance()->AddTransFile(img_name, file_info);
        }

        file_info->_msg_id = message_id;
        file_info->_sender = fromuid;
        file_info->_receiver = touid;
        file_info->_thread_id = thread_id;
        file_info->_transfer_type = TransferType::Download;
        file_info->_transfer_state = TransferState::Downloading;

        auto img_chat_data_ptr = std::make_shared<ImgChatData>(file_info, unique_id,
            thread_id, ChatFormType::PRIVATE, ChatMsgType::PIC,
            fromuid, MsgStatus::READED);

        emit sig_img_chat_msg(img_chat_data_ptr);

		//组织下载请求
		QJsonObject jsonObj_send;
		jsonObj_send["name"] = img_name;
		jsonObj_send["seq"] = file_info->_seq;
		jsonObj_send["trans_size"] = "0";
		jsonObj_send["total_size"] = QString::number(file_info->_total_size);
		//3.2 Resource 鉴权后从 session 取 uid，不再发送 token/uid/sender_id
		jsonObj_send["receiver_id"] = touid;
		jsonObj_send["message_id"] = message_id;

        QDir chatimgDir(img_path_str);
        if (!chatimgDir.exists()) {
            chatimgDir.mkpath(".");
        }

        QJsonDocument doc(jsonObj_send);
        FileTcpMgr::GetInstance()->SendData(ID_IMG_CHAT_DOWN_REQ, doc.toJson());
    } else {
        //文本消息（默认 TEXT）
        auto chat_data = std::make_shared<TextChatData>(message_id, unique_id, thread_id,
            ChatFormType::PRIVATE, ChatMsgType::TEXT, content, fromuid, status, chat_time);
        std::vector<std::shared_ptr<TextChatData>> chat_datas;
        chat_datas.push_back(chat_data);
        emit sig_text_chat_msg(chat_datas);
    }
}

void TcpMgr::loadDeliveryConfig()
{
    //沿用 main.cpp 读取方式：applicationDirPath/config.ini
    QString app_path = QCoreApplication::applicationDirPath();
    QString config_path = QDir::toNativeSeparators(app_path + QDir::separator() + "config.ini");
    QSettings settings(config_path, QSettings::IniFormat);

    bool ok = false;
    qint64 v = settings.value("Delivery/RequestRetryInitialMs", 2000).toLongLong(&ok);
    if (ok && v > 0) {
        _retry_initial_ms = v;
    }
    v = settings.value("Delivery/RequestRetryMaxMs", 30000).toLongLong(&ok);
    if (ok && v > 0) {
        _retry_max_ms = v;
    }
    v = settings.value("Delivery/AckRetryInitialMs", 2000).toLongLong(&ok);
    if (ok && v > 0) {
        _ack_retry_initial_ms = v;
    }
    v = settings.value("Delivery/OfflinePullIntervalMs", 10000).toLongLong(&ok);
    if (ok && v > 0) {
        _offline_pull_interval_ms = static_cast<int>(v);
        if (_offline_pull_timer) {
            _offline_pull_timer->setInterval(_offline_pull_interval_ms);
        }
    }
    v = settings.value("Delivery/OfflinePullBatch", 100).toLongLong(&ok);
    if (ok && v > 0) {
        _offline_pull_batch = static_cast<int>(v);
    }
    qDebug() << "[Delivery] RequestRetryInitialMs=" << _retry_initial_ms
             << " RequestRetryMaxMs=" << _retry_max_ms
             << " AckRetryInitialMs=" << _ack_retry_initial_ms
             << " OfflinePullIntervalMs=" << _offline_pull_interval_ms
             << " OfflinePullBatch=" << _offline_pull_batch;
}

//3.2 一次性净化旧 QSettings 离线队列：删除每条 payload 中的 token 键，无法解析的 record 删除并提示。
//迁移完成后写入 auth_payload_version=2，后续启动跳过。
void TcpMgr::sanitizeLegacyDeliverySettings()
{
    QSettings delivery_settings(QSettings::IniFormat, QSettings::UserScope,
                                "llfc", "llfcchat-delivery");

    //已迁移到 v2，跳过
    if (delivery_settings.value("auth_payload_version").toInt() == 2) {
        return;
    }

    bool dirty = false;       //是否有 token 被删除
    bool dropped_any = false; //是否有无法解析的 record 被删除

    for (const QString& group : delivery_settings.childGroups()) {
        if (!group.startsWith("uid_")) {
            continue;
        }
        delivery_settings.beginGroup(group);
        QString req_str = delivery_settings.value("requests").toString();
        if (req_str.isEmpty()) {
            delivery_settings.endGroup();
            continue;
        }
        QJsonDocument doc = QJsonDocument::fromJson(req_str.toUtf8());
        if (!doc.isArray()) {
            delivery_settings.endGroup();
            continue;
        }
        QJsonArray arr = doc.array();
        QJsonArray cleaned;
        bool group_dirty = false;
        for (int i = 0; i < arr.size(); ++i) {
            QJsonObject obj = arr.at(i).toObject();
            QString payload_str = obj["payload"].toString();
            QJsonDocument pdoc = QJsonDocument::fromJson(payload_str.toUtf8());
            if (pdoc.isNull() || !pdoc.isObject()) {
                //无法解析的 record：删除并记录
                qWarning() << "[Delivery] dropped unparseable pending record in" << group;
                dropped_any = true;
                dirty = true;
                continue;
            }
            QJsonObject payload = pdoc.object();
            if (payload.contains("token")) {
                //删除 token 键（fromuid/touid 等业务字段保留）
                payload.remove("token");
                obj["payload"] = QString::fromUtf8(
                    QJsonDocument(payload).toJson(QJsonDocument::Compact));
                group_dirty = true;
                dirty = true;
            }
            cleaned.append(obj);
        }
        if (group_dirty) {
            delivery_settings.setValue("requests",
                QString::fromUtf8(QJsonDocument(cleaned).toJson(QJsonDocument::Compact)));
        }
        delivery_settings.endGroup();
    }

    if (dirty) {
        delivery_settings.sync();
    }
    //写入 v2 marker（全局顶层 key）
    delivery_settings.setValue("auth_payload_version", 2);
    delivery_settings.sync();

    if (dropped_any) {
        QMessageBox::warning(nullptr, tr("数据迁移"),
            tr("检测到无法解析的旧离线消息记录，已自动删除以保证安全。"));
    }
}

void TcpMgr::slot_send_reliable_chat(ReqId id, QByteArray payload, QStringList unique_ids)
{
    //此 slot 由 queued connection 在 TCP 线程执行
    addPendingRequest(id, payload, unique_ids);

    //立即发送一次（如果已连接）
    if (_socket.state() == QAbstractSocket::ConnectedState) {
        slot_send_data(id, payload);
    }

    //启动重传定时器
    if (!_retry_timer->isActive() && _socket.state() == QAbstractSocket::ConnectedState) {
        _retry_timer->start();
    }
}

void TcpMgr::addPendingRequest(ReqId id, QByteArray payload, QStringList unique_ids)
{
    if (_delivery_uid == 0) {
        auto info = UserMgr::GetInstance()->GetUserInfo();
        if (info) {
            _delivery_uid = info->_uid;
        }
    }

    PendingRequest req;
    req.id = id;
    req.payload = payload;
    req.unique_ids = unique_ids;
    req.retry_delay_ms = _retry_initial_ms;
    req.next_send_epoch_ms = QDateTime::currentMSecsSinceEpoch() + _retry_initial_ms;

    _pending_requests.append(req);
    persistPendingRequests();
}

void TcpMgr::slot_retry_timeout()
{
    if (_socket.state() != QAbstractSocket::ConnectedState) {
        _retry_timer->stop();
        return;
    }

    //sender pending 与 recipient ACK pending 均为空时才停止
    if (_pending_requests.isEmpty() && _pending_ack.isEmpty()) {
        _retry_timer->stop();
        return;
    }

    qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int i = 0; i < _pending_requests.size(); ++i) {
        PendingRequest& req = _pending_requests[i];
        if (req.next_send_epoch_ms <= now) {
            qDebug() << "[Delivery] Retrying id=" << req.id
                     << " unique_ids=" << req.unique_ids
                     << " delay=" << req.retry_delay_ms << "ms";
            slot_send_data(req.id, req.payload);
            //倍增退避，上限 _retry_max_ms
            req.retry_delay_ms = qMin(req.retry_delay_ms * 2, _retry_max_ms);
            req.next_send_epoch_ms = now + req.retry_delay_ms;
        }
    }

    //§6.5：同时扫描待 ACK 项，到期则批量重发 1049
    flushPendingAcks();
}

//§6.5 recipient ACK：批量发送到期项
void TcpMgr::flushPendingAcks()
{
    if (_pending_ack.isEmpty() || _delivery_uid == 0
        || _socket.state() != QAbstractSocket::ConnectedState) {
        return;
    }

    qint64 now = QDateTime::currentMSecsSinceEpoch();
    QJsonArray due_ids;
    QList<int> sent_keys;
    for (auto it = _pending_ack.begin(); it != _pending_ack.end(); ++it) {
        if (it.value().next_send_epoch_ms <= now) {
            due_ids.append(it.key());
            sent_keys.append(it.key());
        }
    }

    if (due_ids.isEmpty()) {
        return;
    }

    QJsonObject obj;
    obj["uid"] = _delivery_uid;
    obj["message_ids"] = due_ids;
    QJsonDocument doc(obj);
    slot_send_data(ID_CHAT_DELIVERY_ACK_REQ, doc.toJson(QJsonDocument::Compact));

    //更新退避与下次发送时刻
    for (int k = 0; k < sent_keys.size(); ++k) {
        AckPending& ap = _pending_ack[sent_keys[k]];
        ap.retry_delay_ms = qMin(ap.retry_delay_ms * 2, _retry_max_ms);
        ap.next_send_epoch_ms = now + ap.retry_delay_ms;
    }

    persistAckPending();
    qDebug() << "[ACK] sent" << sent_keys.size() << "ids, pending_ack=" << _pending_ack.size();
}

void TcpMgr::persistAckPending()
{
    if (_delivery_uid == 0) {
        return;
    }

    QSettings delivery_settings(QSettings::IniFormat, QSettings::UserScope,
                                "llfc", "llfcchat-delivery");
    QString uid_key = QString("uid_%1").arg(_delivery_uid);

    QJsonArray arr;
    for (auto it = _pending_ack.begin(); it != _pending_ack.end(); ++it) {
        QJsonObject o;
        o["message_id"] = it.key();
        o["retry_delay_ms"] = it.value().retry_delay_ms;
        arr.append(o);
    }

    delivery_settings.setValue(uid_key + "/ack_ids",
        QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    delivery_settings.sync();
}

void TcpMgr::loadAckPendingFromDisk(int uid)
{
    QSettings delivery_settings(QSettings::IniFormat, QSettings::UserScope,
                                "llfc", "llfcchat-delivery");
    QString uid_key = QString("uid_%1").arg(uid);
    QString ack_str = delivery_settings.value(uid_key + "/ack_ids").toString();

    if (ack_str.isEmpty()) {
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(ack_str.toUtf8());
    if (!doc.isArray()) {
        return;
    }
    QJsonArray arr = doc.array();
    for (int i = 0; i < arr.size(); ++i) {
        QJsonObject o = arr.at(i).toObject();
        int mid = o["message_id"].toInt();
        if (mid <= 0) {
            continue;
        }
        AckPending ap;
        ap.retry_delay_ms = o["retry_delay_ms"].toInt();
        if (ap.retry_delay_ms <= 0) {
            ap.retry_delay_ms = _ack_retry_initial_ms;
        }
        ap.next_send_epoch_ms = 0; //立即发送
        _pending_ack[mid] = ap;
    }
}

//§6.5 ChatDialog 插入/duplicate 后 emit 此 slot（queued 回 TCP 线程）
void TcpMgr::slot_msg_processed(int message_id)
{
    _pending_ui_ack.remove(message_id);
    if (!_pending_ack.contains(message_id)) {
        AckPending ap;
        ap.retry_delay_ms = _ack_retry_initial_ms;
        ap.next_send_epoch_ms = 0; //立即发送
        _pending_ack[message_id] = ap;
    }
    flushPendingAcks();
    //确保定时器运行（ACK pending 不为空）
    if (!_retry_timer->isActive() && _socket.state() == QAbstractSocket::ConnectedState) {
        _retry_timer->start();
    }
}

//§6.6 离线 pull：启动定时循环
void TcpMgr::slot_start_offline_pull()
{
    if (_socket.state() != QAbstractSocket::ConnectedState || _delivery_uid == 0) {
        return;
    }
    if (!_offline_pull_timer->isActive()) {
        _offline_pull_timer->start();
    }
    //立即触发一次拉取
    slot_offline_pull_timeout();
}

void TcpMgr::slot_offline_pull_timeout()
{
    if (_socket.state() != QAbstractSocket::ConnectedState || _delivery_uid == 0) {
        if (_offline_pull_timer) {
            _offline_pull_timer->stop();
        }
        return;
    }

    //每轮从 after_message_id=0 开始（已 ACK 项已被移除）
    QJsonObject obj;
    obj["uid"] = _delivery_uid;
    obj["after_message_id"] = 0;
    obj["limit"] = _offline_pull_batch;
    QJsonDocument doc(obj);
    slot_send_data(ID_PULL_OFFLINE_MSG_REQ, doc.toJson(QJsonDocument::Compact));
}

void TcpMgr::persistPendingRequests()
{
    if (_delivery_uid == 0) {
        return;
    }

    QSettings delivery_settings(QSettings::IniFormat, QSettings::UserScope,
                                "llfc", "llfcchat-delivery");
    QString uid_key = QString("uid_%1").arg(_delivery_uid);

    QJsonArray arr;
    for (int i = 0; i < _pending_requests.size(); ++i) {
        const PendingRequest& req = _pending_requests[i];
        QJsonObject obj;
        obj["id"] = static_cast<int>(req.id);
        obj["payload"] = QString::fromUtf8(req.payload);
        QJsonArray uid_arr;
        for (int j = 0; j < req.unique_ids.size(); ++j) {
            uid_arr.append(req.unique_ids[j]);
        }
        obj["unique_ids"] = uid_arr;
        arr.append(obj);
    }

    delivery_settings.setValue(uid_key + "/requests",
        QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    delivery_settings.sync();
}

void TcpMgr::restorePendingRequests(int uid)
{
    //§6.2 纠错：本函数只加载持久 pending 到内存（不重建 bubble/MsgInfo、不重发、不启动定时器）。
    //rebuild+重发+定时器由 StartPendingReplay → slot_start_pending_replay 在 GUI thread models
    //建好后执行（ChatDialog::slot_load_chat_thread 最后一页 load_more=false 处调用）。

    //同 uid：pending 已在内存，保留不动（内存为权威）
    if (_delivery_uid == uid) {
        return;
    }

    //切换 uid：清空内存，加载新 uid 的持久 pending（不同账号绝不互载）
    _delivery_uid = uid;
    _pending_requests.clear();
    loadPendingFromDisk(uid);
}

void TcpMgr::loadPendingFromDisk(int uid)
{
    QSettings delivery_settings(QSettings::IniFormat, QSettings::UserScope,
                                "llfc", "llfcchat-delivery");
    QString uid_key = QString("uid_%1").arg(uid);
    QString json_str = delivery_settings.value(uid_key + "/requests").toString();

    if (json_str.isEmpty()) {
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(json_str.toUtf8());
    if (!doc.isArray()) {
        return;
    }
    QJsonArray arr = doc.array();

    for (int i = 0; i < arr.size(); ++i) {
        QJsonObject obj = arr.at(i).toObject();
        PendingRequest req;
        req.id = static_cast<ReqId>(obj["id"].toInt());
        req.payload = obj["payload"].toString().toUtf8();
        req.retry_delay_ms = _retry_initial_ms;
        req.next_send_epoch_ms = 0; //立即发送（由 slot_start_pending_replay 触发）

        QJsonArray uid_arr = obj["unique_ids"].toArray();
        for (int j = 0; j < uid_arr.size(); ++j) {
            req.unique_ids.append(uid_arr.at(j).toString());
        }

        if (req.unique_ids.isEmpty()) {
            continue;
        }

        _pending_requests.append(req);
    }
}

void TcpMgr::slot_start_pending_replay()
{
    //§6.2 纠错：TCP 线程只做解析 → DTO → emit（不在 TCP 线程触碰 ChatThreadData/_msg_unrsp_map
    //或 QPixmap，前者 GUI 可并发访问，后者只能 GUI 线程创建）。
    //GUI 线程收到 sig_replay_pending 后重建 bubble/MsgInfo/QPixmap，再 emit sig_replay_result 回执。
    //回执到达 slot_replay_done 后才删除失效项、persistPendingRequests、重发保留项 + 启动扫描定时器。

    //§6.5：同时恢复 ACK pending（随重启恢复，同一点触发；纯 message_id 状态，无 GUI 对象）
    if (_delivery_uid != 0 && _pending_ack.isEmpty()) {
        loadAckPendingFromDisk(_delivery_uid);
    }

    //解析 sender pending → 跨线程 DTO
    std::vector<TextReplayDTO> texts;
    std::vector<ImageReplayDTO> images;
    for (int i = 0; i < _pending_requests.size(); ++i) {
        const PendingRequest& req = _pending_requests[i];
        QJsonDocument doc = QJsonDocument::fromJson(req.payload);
        QJsonObject obj = doc.object();
        if (req.id == ID_TEXT_CHAT_MSG_REQ) {
            TextReplayDTO dto;
            dto.thread_id = obj["thread_id"].toInt();
            dto.fromuid = obj["fromuid"].toInt();
            QJsonArray arr = obj["text_array"].toArray();
            for (int j = 0; j < arr.size(); ++j) {
                QJsonObject item = arr.at(j).toObject();
                QString uid = item["unique_id"].toString();
                if (req.unique_ids.contains(uid)) {
                    dto.unique_ids.append(uid);
                    dto.contents.append(item["content"].toString());
                }
            }
            if (!dto.unique_ids.isEmpty()) {
                texts.push_back(dto);
            }
        } else if (req.id == ID_IMG_CHAT_MSG_REQ) {
            ImageReplayDTO dto;
            dto.thread_id = obj["thread_id"].toInt();
            dto.fromuid = obj["fromuid"].toInt();
            dto.touid = obj["touid"].toInt();
            dto.name = obj["name"].toString();
            dto.md5 = obj["md5"].toString();
            dto.text_or_url = obj["text_or_url"].toString();
            dto.content_size = 0;
            if (obj.contains("content_size")) {
                dto.content_size = obj["content_size"].toString().toLongLong();
            }
            dto.unique_id = req.unique_ids.isEmpty() ? QString() : req.unique_ids.first();
            images.push_back(dto);
        }
    }

    //总是 emit（即使两个列表都为空，GUI 仍会回执以便 TCP 完成 ACK flush + 定时器启动）
    emit sig_replay_pending(texts, images);
}

void TcpMgr::removePendingByUniqueId(const QString& unique_id)
{
    if (unique_id.isEmpty()) {
        return;
    }

    for (int i = _pending_requests.size() - 1; i >= 0; --i) {
        PendingRequest& req = _pending_requests[i];
        if (req.unique_ids.removeAll(unique_id) > 0) {
            if (req.unique_ids.isEmpty()) {
                _pending_requests.removeAt(i);
            }
        }
    }
}

void TcpMgr::handleTextConflict(int thread_id, int fromuid, const QStringList& conflict_ids)
{
    //批量回滚且无法映射单个 item：conflict_ids 为空时整批停止标失败
    bool batch_rollback = conflict_ids.isEmpty();
    bool has_thread_info = (thread_id > 0);

    std::vector<std::shared_ptr<TextChatData>> failed;
    int emit_thread_id = 0;

    for (int i = _pending_requests.size() - 1; i >= 0; --i) {
        PendingRequest& req = _pending_requests[i];
        if (req.id != ID_TEXT_CHAT_MSG_REQ) {
            continue;
        }

        QJsonDocument doc = QJsonDocument::fromJson(req.payload);
        QJsonObject obj = doc.object();
        int req_thread = obj["thread_id"].toInt();
        int req_from = obj["fromuid"].toInt();

        //按 thread_id 过滤（有信息时）
        if (has_thread_info && req_thread != thread_id) {
            continue;
        }

        //确定本请求中哪些 unique_id 要标失败
        QStringList fail_in_this_req;
        if (batch_rollback) {
            fail_in_this_req = req.unique_ids; //整批
            if (emit_thread_id == 0) {
                emit_thread_id = req_thread;
            }
        } else {
            for (int j = 0; j < req.unique_ids.size(); ++j) {
                if (conflict_ids.contains(req.unique_ids[j])) {
                    fail_in_this_req.append(req.unique_ids[j]);
                }
            }
        }

        if (fail_in_this_req.isEmpty()) {
            continue;
        }

        //构造失败 TextChatData 并从 pending 移除
        QJsonArray text_arr = obj["text_array"].toArray();
        for (int j = 0; j < text_arr.size(); ++j) {
            QJsonObject item = text_arr.at(j).toObject();
            QString item_uid = item["unique_id"].toString();
            if (!fail_in_this_req.contains(item_uid)) {
                continue;
            }

            auto msg = std::make_shared<TextChatData>(
                item_uid, req_thread,
                ChatFormType::PRIVATE, ChatMsgType::TEXT,
                item["content"].toString(), req_from,
                MsgStatus::SEND_FAILED);
            failed.push_back(msg);
            if (emit_thread_id == 0) {
                emit_thread_id = req_thread;
            }
            req.unique_ids.removeAll(item_uid);
        }

        if (req.unique_ids.isEmpty()) {
            _pending_requests.removeAt(i);
        }
    }

    //通过现有 sig_chat_msg_rsp 信号路径标 SEND_FAILED（MoveMsg + UpdateChatStatus）
    if (!failed.empty()) {
        emit sig_chat_msg_rsp(emit_thread_id, failed);
    }

    persistPendingRequests();
}

void TcpMgr::handleImageConflict(const QString& conflict_id)
{
    if (conflict_id.isEmpty()) {
        return;
    }

    for (int i = _pending_requests.size() - 1; i >= 0; --i) {
        PendingRequest& req = _pending_requests[i];
        if (req.id != ID_IMG_CHAT_MSG_REQ) {
            continue;
        }
        if (!req.unique_ids.contains(conflict_id)) {
            continue;
        }

        QJsonDocument doc = QJsonDocument::fromJson(req.payload);
        QJsonObject obj = doc.object();
        int thread_id = obj["thread_id"].toInt();
        int fromuid = obj["fromuid"].toInt();
        QString name = obj["name"].toString();

        //通过现有 sig_chat_img_rsp 信号路径标 SEND_FAILED
        auto file_info = UserMgr::GetInstance()->GetTransFileByName(name);
        if (file_info) {
            auto img_msg = std::make_shared<ImgChatData>(
                file_info, conflict_id, thread_id,
                ChatFormType::PRIVATE, ChatMsgType::PIC,
                fromuid, MsgStatus::SEND_FAILED);
            emit sig_chat_img_rsp(thread_id, img_msg);
        }

        req.unique_ids.removeAll(conflict_id);
        if (req.unique_ids.isEmpty()) {
            _pending_requests.removeAt(i);
        }
        break;
    }

    persistPendingRequests();
}

//§6.2 纠错：GUI 重建完成回执（queued 回 TCP 线程）。
//删除文件缺失的失效项、persistPendingRequests、重发保留项 + 启动 250ms 扫描定时器。
//ACK pending 恢复已在 slot_start_pending_replay 加载，此处 flush + 启动定时器。
void TcpMgr::slot_replay_done(QStringList failed_unique_ids)
{
    //删除失效项（GUI 已标 SEND_FAILED）
    for (int i = 0; i < failed_unique_ids.size(); ++i) {
        removePendingByUniqueId(failed_unique_ids[i]);
    }
    persistPendingRequests();

    //重发所有保留的 sender pending（GUI 已重建 bubble/MsgInfo，可安全重发）
    if (_socket.state() == QAbstractSocket::ConnectedState) {
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (int i = 0; i < _pending_requests.size(); ++i) {
            PendingRequest& req = _pending_requests[i];
            slot_send_data(req.id, req.payload);
            req.next_send_epoch_ms = now + _retry_initial_ms;
        }
    }

    //§6.5：重发恢复的 ACK pending
    if (!_pending_ack.isEmpty()) {
        flushPendingAcks();
    }

    //启动定时器（sender pending 或 ACK pending 任一非空）
    if (!_retry_timer->isActive() && _socket.state() == QAbstractSocket::ConnectedState
        && (!_pending_requests.isEmpty() || !_pending_ack.isEmpty())) {
        _retry_timer->start();
    }
}

TcpThread::TcpThread()
{
    _tcp_thread = new QThread();
    TcpMgr::GetInstance()->moveToThread(_tcp_thread);
    QObject::connect(_tcp_thread, &QThread::finished, _tcp_thread, &QObject::deleteLater);

    _tcp_thread->start();
}

TcpThread::~TcpThread()
{
    _tcp_thread->quit();
}
