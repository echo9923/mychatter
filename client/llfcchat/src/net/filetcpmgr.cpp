#include "filetcpmgr.h"
#include "usermgr.h"
#include <QPainter>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>

//offset(字节) 对应的已确认分片序号：非末片 server_offset 恒为 32K 整数倍
static qint64 OffsetToChunkIndex(qint64 server_offset, qint64 total_size, qint64 max_seq)
{
    if (server_offset >= total_size) {
        return max_seq;
    }
    return server_offset / MAX_FILE_LEN;
}

//单个内存分片的 SHA-256（hex），下载逐片校验用
static QString calculateChunkSha256(const QByteArray& chunk)
{
    if (chunk.isEmpty()) {
        return QString();
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(chunk);
    return QString::fromLatin1(hash.result().toHex().toLower());
}

FileTcpMgr::FileTcpMgr(QObject* parent) : QObject(parent),
_host(""), _port(0), _b_recv_pending(false), _message_type(0), _message_len(0),
_bytes_sent(0), _pending(false), _authenticated(false), _cwnd_size(0)
{
    registerMetaType();
    QObject::connect(&_socket, &QTcpSocket::connected, this, [&]() {
        qDebug() << "Connected to server!";
        //连接起点：新连接不允许继承上一连接的鉴权态，发 1501 登录帧前重置
        _authenticated = false;
        emit sig_con_success(true);
        });


    QObject::connect(&_socket, &QTcpSocket::readyRead, this, [&]() {
        // 当有数据可读时，读取所有数据
        // 读取所有数据并追加到缓冲区
        _buffer.append(_socket.readAll());

        forever{
            //先解析头部
           if (!_b_recv_pending) {
               // 检查缓冲区中的数据是否足够解析出一个消息头（消息类型 + 消息长度）
               if (_buffer.size() < FILE_UPLOAD_HEAD_LEN) {
                   return; // 数据不够，等待更多数据
               }

               QDataStream stream(_buffer);
               stream.setVersion(QDataStream::Qt_5_0);
               stream >> _message_type >> _message_len;

               _buffer.remove(0, FILE_UPLOAD_HEAD_LEN);  // 使用remove代替mid赋值

               qDebug() << "Message Type:" << _message_type << ", Length:" << _message_len;

           }

        //buffer剩余长读是否满足消息体长度，不满足则退出继续等待接受
       if (_buffer.size() < _message_len) {
            _b_recv_pending = true;
            return;
       }

       _b_recv_pending = false;
       // 读取消息体
       QByteArray messageBody = _buffer.mid(0, _message_len);
       qDebug() << "receive body msg is " << messageBody;

       _buffer = _buffer.mid(_message_len);
       handleMsg(ReqId(_message_type),_message_len, messageBody);
        }

        });

    // 处理错误（适用于Qt 5.15之前的版本）
    QObject::connect(&_socket, static_cast<void (QTcpSocket::*)(QTcpSocket::SocketError)>(&QTcpSocket::error),
        this,
        [&](QTcpSocket::SocketError socketError) {
            qDebug() << "Error:" << _socket.errorString();
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
                //qDebug() << "Network Error!";
                break;
            default:
                //qDebug() << "Other Error!";
                break;
            }
        });

    // 处理连接断开
    QObject::connect(&_socket, &QTcpSocket::disconnected, this, [&]() {
        qDebug() << "Disconnected from server.";
        //断线重置鉴权态，避免重连后误以为已登录
        _authenticated = false;
        emit sig_connection_closed();
        });


    //连接发送信号用来发送数据
    QObject::connect(this, &FileTcpMgr::sig_send_data, this, &FileTcpMgr::slot_send_data);

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

    //连接
    QObject::connect(this, &FileTcpMgr::sig_close, this, &FileTcpMgr::slot_tcp_close);

    //链接续传信号
    QObject::connect(this, &FileTcpMgr::sig_continue_upload_file, this, &FileTcpMgr::slot_continue_upload_file);
    QObject::connect(this, &FileTcpMgr::sig_continue_download_file, this, &FileTcpMgr::slot_continue_download_file);
    //资源下载启动（跨线程投递统一走信号）
    QObject::connect(this, &FileTcpMgr::sig_start_resource_download, this, &FileTcpMgr::slot_start_resource_download);
    //跨线程 BatchSend 投递（_cwnd_size 只在 File 线程读写）
    QObject::connect(this, &FileTcpMgr::sig_batch_send, this, &FileTcpMgr::slot_batch_send);
    //注册消息
    initHandlers();

}

void FileTcpMgr::registerMetaType()
{
    // 注册所有自定义类型
    qRegisterMetaType<ServerInfo>("ServerInfo");
    qRegisterMetaType<std::shared_ptr<ServerInfo>>("std::shared_ptr<ServerInfo>");
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
    qRegisterMetaType<MsgInfo>("MsgInfo");
    qRegisterMetaType<std::shared_ptr<MsgInfo>>("std::shared_ptr<MsgInfo>");
}

void FileTcpMgr::handleMsg(ReqId id, int len, QByteArray data)
{
    auto find_iter = _handlers.find(id);
    if (find_iter == _handlers.end()) {
        qDebug() << "not found id [" << id << "] to handle";
        return;
    }

    (this->*find_iter.value())(id, len, data);
}

void FileTcpMgr::slot_send_data(ReqId reqId, QByteArray dataBytes)
{
    //3.2 鉴权前禁止发送任何业务帧（仅允许 1501 登录请求），未鉴权直接丢弃并记日志
    if (!_authenticated && reqId != ID_RESOURCE_LOGIN_REQ) {
        qWarning() << "[FileTcpMgr] dropping frame id=" << reqId
                   << " before resource authentication";
        return;
    }

    uint16_t id = reqId;

    // 计算长度（使用网络字节序转换）
    quint32 len = static_cast<quint32>(dataBytes.length());

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
    _bytes_sent = 0;               // ← 归零
    _pending = true;               // ← 标记正在发送

    qint64 written = _socket.write(_current_block);
    Q_UNUSED(written);
}

void FileTcpMgr::slot_tcp_connect(std::shared_ptr<ServerInfo> si)
{
    qDebug() << "receive tcp connect signal";
    // 尝试连接到服务器
    qDebug() << "Connecting to server...";
    _host = si->_res_host;
    _port = static_cast<uint16_t>(si->_res_port.toUInt());
    _socket.connectToHost(_host, _port);
}


FileTcpMgr::~FileTcpMgr() {
    //析构：确保鉴权态不残留
    _authenticated = false;
}

void FileTcpMgr::SendData(ReqId reqId, QByteArray data)
{
    emit sig_send_data(reqId, data);
}

void FileTcpMgr::initHandlers()
{
    //3.2 Resource 登录回复（1502）：鉴权成功后才允许发送业务帧
    _handlers.insert(ID_RESOURCE_LOGIN_RSP, &FileTcpMgr::handleResourceLoginRsp);

    // ---------- 头像通道（1601/1602/1603/1604，旧协议保持不变） ----------
    // 接收上传用户头像回复
    _handlers.insert(ID_UPLOAD_HEAD_ICON_RSP, &FileTcpMgr::handleUploadHeadIconRsp);

    _handlers.insert(ID_DOWN_LOAD_FILE_RSP, &FileTcpMgr::handleDownloadFileRsp);

    // ---------- 资源上传通道（1507/1508 + 1509/1510，offset+SHA-256） ----------

    //1508 上传分片回包：{error, message_id:"<str>", server_offset:"<str>", resource_status}
    _handlers.insert(ID_RESOURCE_CHUNK_UPLOAD_RSP, &FileTcpMgr::handleResourceChunkUploadRsp);

    //1510 上传进度查询回包（OutboxDispatcher 重启/重连恢复续传用）
    _handlers.insert(ID_RESOURCE_UPLOAD_PROGRESS_RSP, &FileTcpMgr::handleResourceUploadProgressRsp);

    // ---------- 资源下载通道（1511/1512 + 1513/1514，offset+SHA-256） ----------

    //1512 下载信息回包：{error, message_id, file_name, total_size, content_hash, mime_type,
    //msg_type, resource_status}
    _handlers.insert(ID_RESOURCE_DOWN_INFO_RSP, &FileTcpMgr::handleResourceDownInfoRsp);

    //1514 分片下载回包：{error, message_id, offset, bytes, chunk_sha256, data, total_size, is_last}
    _handlers.insert(ID_RESOURCE_CHUNK_DOWN_RSP, &FileTcpMgr::handleResourceChunkDownRsp);
}

void FileTcpMgr::handleResourceLoginRsp(ReqId id, int len, QByteArray data)
{
    Q_UNUSED(len);
    Q_UNUSED(id);
    QJsonDocument jsonDoc = QJsonDocument::fromJson(data);
    if (jsonDoc.isNull() || !jsonDoc.isObject()) {
        _authenticated = false;
        emit sig_resource_login_failed(tr("资源服务器鉴权响应解析失败"));
        return;
    }
    QJsonObject jsonObj = jsonDoc.object();
    int err = jsonObj.contains("error") ? jsonObj["error"].toInt() : ErrorCodes::ERR_JSON;
    if (err != ErrorCodes::SUCCESS) {
        _authenticated = false;
        qDebug() << "[FileTcpMgr] resource login failed, error=" << err;
        emit sig_resource_login_failed(tr("资源服务器鉴权失败"));
        return;
    }
    _authenticated = true;
    qDebug() << "[FileTcpMgr] resource login success";
    emit sig_resource_login_success();
}

void FileTcpMgr::handleUploadHeadIconRsp(ReqId id, int len, QByteArray data)
{
    Q_UNUSED(len);
    qDebug() << "handle id is " << id;
    // 将QByteArray转换为QJsonDocument
    QJsonDocument jsonDoc = QJsonDocument::fromJson(data);

    // 检查转换是否成功
    if (jsonDoc.isNull()) {
        qDebug() << "Failed to create QJsonDocument.";
        return;
    }

    QJsonObject recvObj = jsonDoc.object();

    if (!recvObj.contains("error")) {
        qDebug() << "icon upload_failed, err is Json Parse Err";
        return;
    }

    int err = recvObj["error"].toInt();
    if (err != ErrorCodes::SUCCESS) {
        qDebug() << "icon upload failed, err is " << err;
        return;
    }

    auto md5 = recvObj["md5"].toString();
    auto seq = recvObj["seq"].toInt();
    auto trans_size = recvObj["trans_size"].toInt();
    auto total_size = recvObj["total_size"].toInt();
    auto name = recvObj["name"].toString();

    //判断trans_size和total_size相等
    if (total_size == trans_size) {
        UserMgr::GetInstance()->RmvUploadFile(name);
        return;
    }

    auto file_info = UserMgr::GetInstance()->GetUploadInfoByName(name);
    if (!file_info) {
        return;
    }

    //再次组织数据发送
    QFile file(file_info->filePath());
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Could not open file: " << file.errorString();
        return;
    }

    //文件偏移到已经发送的位置，继续读取发送
    file.seek(trans_size);
    QByteArray buffer;
    seq++;
    //每次读取MAX_FILE_LEN字节发送
    buffer = file.read(MAX_FILE_LEN);
    QJsonObject sendObj;
    //将文件内容转换为base64编码
    QString base64Data = buffer.toBase64();
    sendObj["md5"] = md5;
    sendObj["name"] = name;
    sendObj["seq"] = seq;
    sendObj["trans_size"] = buffer.size() + (seq - 1) * MAX_FILE_LEN;
    sendObj["total_size"] = total_size;

    if (buffer.size() + (seq - 1) * MAX_FILE_LEN >= total_size) {
        sendObj["last"] = 1;
    }
    else {
        sendObj["last"] = 0;
    }

    sendObj["data"] = base64Data;
    sendObj["last_seq"] = recvObj["last_seq"].toInt();
    QJsonDocument doc(sendObj);
    auto send_data = doc.toJson();
    SendData(ID_UPLOAD_HEAD_ICON_REQ, send_data);

    file.close();
}

void FileTcpMgr::handleDownloadFileRsp(ReqId id, int len, QByteArray data)
{
    Q_UNUSED(len);
    qDebug() << "handle id is " << id;
    QJsonDocument jsonDoc = QJsonDocument::fromJson(data);

    if (jsonDoc.isNull()) {
        qDebug() << "Failed to create QJsonDocument.";
        return;
    }

    QJsonObject jsonObj = jsonDoc.object();

    if (!jsonObj.contains("error")) {
        qDebug() << "avatar download json parse failed";
        return;
    }

    int err = jsonObj["error"].toInt();
    if (err != ErrorCodes::SUCCESS) {
        qDebug() << "avatar download failed, error is " << err;
        return;
    }

    QString base64Data = jsonObj["data"].toString();
    QString clientPath = jsonObj["client_path"].toString();
    int seq = jsonObj["seq"].toInt();
    bool is_last = jsonObj["is_last"].toBool();
    QString total_size_str = jsonObj["total_size"].toString();
    qint64  total_size = total_size_str.toLongLong(nullptr);
    QString current_size_str = jsonObj["current_size"].toString();
    qint64  current_size = current_size_str.toLongLong(nullptr);
    QString name = jsonObj["name"].toString();
    QString req_type = jsonObj["req_type"].toString();

    auto file_info = UserMgr::GetInstance()->GetDownloadInfo(name);
    if (file_info == nullptr) {
        qDebug() << "file: " << name << " not found";
        return;
    }

    file_info->_current_size = current_size;
    file_info->_total_size = total_size;

    //Base64解码
    QByteArray decodedData = QByteArray::fromBase64(base64Data.toUtf8());
    QFile file(clientPath);

    // 根据 seq 决定打开模式
    QIODevice::OpenMode mode;
    if (seq == 1) {
        mode = QIODevice::WriteOnly;
    }
    else {
        mode = QIODevice::WriteOnly | QIODevice::Append;
    }

    if (!file.open(mode)) {
        qDebug() << "Failed to open file for writing:" << clientPath;
        return;
    }

    qint64 bytesWritten = file.write(decodedData);
    if (bytesWritten != decodedData.size()) {
        qDebug() << "Failed to write all data. Written:" << bytesWritten
            << "Expected:" << decodedData.size();
    }

    file.close();

    if (is_last) {
        UserMgr::GetInstance()->RmvDownloadFile(name);
        if (req_type == "self_icon") {
            //发送信号通知主界面重新加载label
            emit sig_reset_label_icon(clientPath);
        }
    }
    else {
        //继续请求
        file_info->_seq = seq + 1;
        FileTcpMgr::GetInstance()->SendDownloadInfo(file_info, req_type);
    }
}

void FileTcpMgr::handleResourceChunkUploadRsp(ReqId id, int len, QByteArray data)
{
    Q_UNUSED(len);
    Q_UNUSED(id);
    _cwnd_size--;
    if (_cwnd_size < 0) {
        _cwnd_size = 0;
    }
    QJsonDocument jsonDoc = QJsonDocument::fromJson(data);
    if (jsonDoc.isNull() || !jsonDoc.isObject()) {
        qDebug() << "[FileTcpMgr] 1508 json parse failed";
        return;
    }
    QJsonObject recvObj = jsonDoc.object();
    if (!recvObj.contains("error")) {
        return;
    }

    const qint64 message_id = recvObj["message_id"].toVariant().toLongLong();
    const int err = recvObj["error"].toInt();
    const qint64 server_offset = recvObj["server_offset"].toVariant().toLongLong();
    const int resource_status = recvObj.contains("resource_status")
        ? recvObj["resource_status"].toInt() : -1;
    auto file_info = UserMgr::GetInstance()->GetTransFileByMsgId(message_id);
    if (!file_info) {
        qDebug() << "[FileTcpMgr] 1508 for unknown message " << message_id;
        return;
    }
    const QString name = file_info->_unique_name;

    //永久失败：无权/终态，停止重传
    if (err == ErrorCodes::RESOURCE_FORBIDDEN || err == ErrorCodes::RESOURCE_STATE_INVALID) {
        qWarning() << "[FileTcpMgr] upload permanent failed msg=" << message_id
            << " err=" << err;
        UserMgr::GetInstance()->RmvTransFileByName(name);
        emit sig_resource_upload_failed(name, err);
        return;
    }

    if (err == ErrorCodes::SUCCESS) {
        //按服务端真值推进确认偏移（重复片幂等确认也走这里）
        const qint64 confirmed = OffsetToChunkIndex(server_offset,
            file_info->_total_size, file_info->_max_seq);
        if (confirmed > file_info->_last_confirmed_seq) {
            file_info->_last_confirmed_seq = confirmed;
        }
        file_info->_rsp_seqs.clear();
        file_info->_flighting_seqs.clear();
        file_info->_seq = file_info->_last_confirmed_seq + 1;
        file_info->_rsp_size = qMin(server_offset, file_info->_total_size);

        if (resource_status == RESOURCE_READY
            || file_info->_rsp_size >= file_info->_total_size) {
            //上传收全：发送方本地归档一份到资源目录，通知 UI/Outbox
            auto uid = UserMgr::GetInstance()->GetUid();
            QString storageDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
            QString res_dir = storageDir + "/user/" + QString::number(uid)
                + "/resources/" + QString::number(file_info->_sender);
            QString dest_path = res_dir + '/' + QString::number(message_id);
            CopyFile(file_info->_text_or_url, dest_path, res_dir);
            file_info->_transfer_state = TransferState::Completed;
            emit sig_update_upload_progress(file_info);
            emit sig_resource_upload_done(name);
            UserMgr::GetInstance()->RmvTransFileByName(name);
            //窗口腾出后继续下一个排队文件（由 OutboxDispatcher 重新驱动，不再自动轮转）
            return;
        }

        emit sig_update_upload_progress(file_info);
        //暂停状态不继续发
        if (file_info->_transfer_state == TransferState::Paused) {
            return;
        }
        BatchSend(file_info);
        return;
    }

    //2107 偏移超前 / 2112 片或整文件哈希不符：一律按服务端 server_offset 对齐重发
    //(整文件 2112 时服务端已删 .part，server_offset=0，即整文件重传)
    qWarning() << "[FileTcpMgr] upload chunk error=" << err << " msg=" << message_id
        << " server_offset=" << server_offset;
    const qint64 confirmed = OffsetToChunkIndex(server_offset,
        file_info->_total_size, file_info->_max_seq);
    file_info->_rsp_seqs.clear();
    file_info->_flighting_seqs.clear();
    file_info->_last_confirmed_seq = confirmed;
    file_info->_seq = confirmed + 1;
    file_info->_rsp_size = qMin(server_offset, file_info->_total_size);
    if (file_info->_transfer_state == TransferState::Paused) {
        return;
    }
    BatchSend(file_info);
}

void FileTcpMgr::handleResourceUploadProgressRsp(ReqId id, int len, QByteArray data)
{
    Q_UNUSED(len);
    Q_UNUSED(id);
    QJsonDocument jsonDoc = QJsonDocument::fromJson(data);
    if (jsonDoc.isNull() || !jsonDoc.isObject()) {
        return;
    }
    QJsonObject recvObj = jsonDoc.object();
    emit sig_upload_progress_rsp(
        recvObj["message_id"].toVariant().toLongLong(),
        recvObj.contains("error") ? recvObj["error"].toInt() : ErrorCodes::ERR_JSON,
        recvObj["server_offset"].toVariant().toLongLong(),
        recvObj.contains("resource_status") ? recvObj["resource_status"].toInt() : -1);
}

void FileTcpMgr::handleResourceDownInfoRsp(ReqId id, int len, QByteArray data)
{
    Q_UNUSED(len);
    Q_UNUSED(id);
    QJsonDocument jsonDoc = QJsonDocument::fromJson(data);
    if (jsonDoc.isNull() || !jsonDoc.isObject()) {
        return;
    }
    QJsonObject jsonObj = jsonDoc.object();
    if (!jsonObj.contains("error")) {
        return;
    }
    const qint64 message_id = jsonObj["message_id"].toVariant().toLongLong();
    const int err = jsonObj["error"].toInt();
    auto file_info = UserMgr::GetInstance()->GetTransFileByMsgId(message_id);
    if (!file_info) {
        return;
    }

    if (err == ErrorCodes::RESOURCE_NOT_READY) {
        //资源尚未就绪：保留占位图，稍后可重试
        qDebug() << "[FileTcpMgr] resource not ready msg=" << message_id;
        return;
    }
    if (err != ErrorCodes::SUCCESS) {
        //2115 无权 / 2116 已过期 / 2101 文件缺失：终态
        qWarning() << "[FileTcpMgr] down info failed msg=" << message_id << " err=" << err;
        file_info->_transfer_state = (err == ErrorCodes::RESOURCE_STATE_INVALID)
            ? TransferState::Expired : TransferState::Failed;
        emit sig_download_failed(file_info, err);
        return;
    }

    //元数据就位（file_name 为原始文件名，仅展示；缓存按 message_id 隔离）
    file_info->_total_size = jsonObj["total_size"].toString().toLongLong();
    file_info->_content_hash = jsonObj["content_hash"].toString();
    file_info->_max_seq = (file_info->_total_size + MAX_FILE_LEN - 1) / MAX_FILE_LEN;
    const QString cache_dir = resourceCacheDir(message_id);
    QDir dir(cache_dir);
    if (!dir.exists()) {
        dir.mkpath(".");
    }
    const QString file_name = jsonObj["file_name"].toString();
    const QString part_path = cache_dir + '/' + file_name + ".part";

    //从本地 .part 实际大小续传（首传为 0）
    QFile part(part_path);
    qint64 offset = 0;
    if (part.exists()) {
        offset = part.size();
        if (offset >= file_info->_total_size) {
            offset = 0; //异常残留：重头来
        }
    }
    file_info->_transfer_type = TransferType::Download;
    if (file_info->_transfer_state != TransferState::Paused) {
        file_info->_transfer_state = TransferState::Downloading;
    }
    file_info->_current_size = offset;
    file_info->_rsp_size = offset;
    emit sig_update_download_progress(file_info);

    if (file_info->_transfer_state == TransferState::Paused) {
        return;
    }
    QJsonObject req;
    req["message_id"] = QString::number(message_id);
    req["offset"] = QString::number(offset);
    SendData(ID_RESOURCE_CHUNK_DOWN_REQ, QJsonDocument(req).toJson(QJsonDocument::Compact));
}

void FileTcpMgr::handleResourceChunkDownRsp(ReqId id, int len, QByteArray data)
{
    Q_UNUSED(len);
    Q_UNUSED(id);
    QJsonDocument jsonDoc = QJsonDocument::fromJson(data);
    if (jsonDoc.isNull() || !jsonDoc.isObject()) {
        return;
    }
    QJsonObject jsonObj = jsonDoc.object();
    if (!jsonObj.contains("error")) {
        return;
    }
    const qint64 message_id = jsonObj["message_id"].toVariant().toLongLong();
    const int err = jsonObj["error"].toInt();
    auto file_info = UserMgr::GetInstance()->GetTransFileByMsgId(message_id);
    if (!file_info) {
        return;
    }

    if (err == ErrorCodes::FILE_OFFSET_INVALID) {
        //本地 .part 与服务端分叉：删除后从 0 重下
        const QString cache_dir = resourceCacheDir(message_id);
        QFile::remove(cache_dir + '/' + file_info->_unique_name + ".part");
        slot_start_resource_download(file_info);
        return;
    }
    if (err == ErrorCodes::RESOURCE_NOT_READY) {
        return;
    }
    if (err != ErrorCodes::SUCCESS) {
        file_info->_transfer_state = (err == ErrorCodes::RESOURCE_STATE_INVALID)
            ? TransferState::Expired : TransferState::Failed;
        emit sig_download_failed(file_info, err);
        return;
    }

    const qint64 offset = jsonObj["offset"].toVariant().toLongLong();
    const qint64 bytes = jsonObj["bytes"].toVariant().toLongLong();
    const bool is_last = jsonObj["is_last"].toBool();
    const QString chunk_sha = jsonObj["chunk_sha256"].toString();
    const QByteArray decoded = QByteArray::fromBase64(jsonObj["data"].toString().toUtf8());

    //逐片校验：与分片等长 + 哈希一致才落盘
    const QString cache_dir = resourceCacheDir(message_id);
    const QString file_name = file_info->_unique_name;
    const QString part_path = cache_dir + '/' + file_name + ".part";
    if (decoded.size() != static_cast<int>(bytes)
        || calculateChunkSha256(decoded) != chunk_sha) {
        qWarning() << "[FileTcpMgr] chunk hash mismatch msg=" << message_id
            << " offset=" << offset << ", restart download";
        QFile::remove(part_path);
        slot_start_resource_download(file_info);
        return;
    }

    //写 .part：seek 到 offset 精确覆盖（重复片幂等）
    QFile part(part_path);
    if (!part.open(QIODevice::ReadWrite)) {
        qWarning() << "[FileTcpMgr] open part failed:" << part.errorString();
        file_info->_transfer_state = TransferState::Failed;
        emit sig_download_failed(file_info, ErrorCodes::ERR_NETWORK);
        return;
    }
    part.seek(offset);
    if (part.write(decoded) != decoded.size()) {
        part.close();
        file_info->_transfer_state = TransferState::Failed;
        emit sig_download_failed(file_info, ErrorCodes::ERR_NETWORK);
        return;
    }
    part.close();

    file_info->_current_size = offset + bytes;
    file_info->_rsp_size = offset + bytes;
    file_info->_seq = file_info->_current_size / MAX_FILE_LEN + 1;
    file_info->_last_confirmed_seq = file_info->_seq - 1;
    emit sig_update_download_progress(file_info);

    if (!is_last) {
        if (file_info->_transfer_state == TransferState::Paused) {
            return;
        }
        QJsonObject req;
        req["message_id"] = QString::number(message_id);
        req["offset"] = QString::number(offset + bytes);
        SendData(ID_RESOURCE_CHUNK_DOWN_REQ,
            QJsonDocument(req).toJson(QJsonDocument::Compact));
        return;
    }

    //收齐：整文件 SHA-256 校验通过后原子改名
    const QString final_path = cache_dir + '/' + file_name;
    const QString whole = calculateFileSha256Only(part_path);
    if (whole.isEmpty() || (!file_info->_content_hash.isEmpty() && whole != file_info->_content_hash)) {
        qWarning() << "[FileTcpMgr] whole-file hash mismatch msg=" << message_id
            << ", restart download";
        QFile::remove(part_path);
        file_info->_transfer_state = TransferState::Failed;
        emit sig_download_failed(file_info, ErrorCodes::FILE_HASH_MISMATCH);
        return;
    }
    QFile::remove(final_path); //残留先清
    if (!QFile::rename(part_path, final_path)) {
        qWarning() << "[FileTcpMgr] finalize failed msg=" << message_id;
        file_info->_transfer_state = TransferState::Failed;
        emit sig_download_failed(file_info, ErrorCodes::ERR_NETWORK);
        return;
    }
    file_info->_transfer_state = TransferState::Completed;
    file_info->_local_download_path = final_path;
    file_info->_current_size = file_info->_total_size;
    UserMgr::GetInstance()->RmvTransFileByName(file_name);
    emit sig_download_finish(file_info, final_path);
}

QString FileTcpMgr::resourceCacheDir(qint64 message_id)
{
    //下载缓存按 message_id 隔离，避免同名文件互相覆盖
    auto uid = UserMgr::GetInstance()->GetUid();
    QString storageDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return storageDir + "/user/" + QString::number(uid)
        + "/cache/" + QString::number(message_id);
}

void FileTcpMgr::CopyFile(QString src_path, QString dst_path, QString dst_dir) {
    //将文件移动到用户自己的资源目录

    QDir resDir(dst_dir);
    if (!resDir.exists()) {
        resDir.mkpath(".");
    }

    if (QFile::copy(src_path, dst_path)) {
        qDebug() << "文件拷贝成功";
    }
    else {
        qDebug() << "文件拷贝失败";
    }
}

void FileTcpMgr::StartResourceDownload(std::shared_ptr<MsgInfo> msg_info) {
    if (!msg_info || msg_info->_msg_id <= 0) {
        return;
    }
    UserMgr::GetInstance()->AddTransFile(msg_info->_unique_name, msg_info);
    emit sig_start_resource_download(msg_info);
}

void FileTcpMgr::slot_start_resource_download(std::shared_ptr<MsgInfo> msg_info) {
    if (!msg_info || msg_info->_msg_id <= 0) {
        return;
    }
    //1511 查元数据（含权限/就绪校验），1512 回包里接续 1513 逐片下载
    QJsonObject req;
    req["message_id"] = QString::number(msg_info->_msg_id);
    SendData(ID_RESOURCE_DOWN_INFO_REQ, QJsonDocument(req).toJson(QJsonDocument::Compact));
}

void FileTcpMgr::ContinueUploadFile(QString unique_name) {
    emit sig_continue_upload_file(unique_name);
}

void FileTcpMgr::ContinueDownloadFile(QString unique_name) {
    emit sig_continue_download_file(unique_name);
}

void FileTcpMgr::BatchSend(std::shared_ptr<MsgInfo> msg_info) {
    if (msg_info == nullptr) {
        return;
    }

    //已确认偏移 == 总大小：发完了
    if (msg_info->_last_confirmed_seq >= msg_info->_max_seq) {
        qDebug() << "file has sent finished";
        return;
    }

    if (MAX_CWND_SIZE - _cwnd_size == 0) {
        return;
    }

    //片哈希必须已预计算（MessageTextEdit 采集时完成）
    if (msg_info->_chunk_hashes.isEmpty()) {
        QString hash;
        QVector<QString> chunk_hashes;
        if (calculateFileSha256(msg_info->_text_or_url, hash, chunk_hashes)) {
            msg_info->_content_hash = hash;
            msg_info->_chunk_hashes = chunk_hashes;
        }
        else {
            qWarning() << "[FileTcpMgr] source missing or unreadable:" << msg_info->_text_or_url;
            emit sig_resource_upload_failed(msg_info->_unique_name, ErrorCodes::ERR_NETWORK);
            return;
        }
    }

    //打开
    QFile file(msg_info->_text_or_url);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Could not open file: " << file.errorString();
        emit sig_resource_upload_failed(msg_info->_unique_name, ErrorCodes::ERR_NETWORK);
        return;
    }

    //从下一个未确认分片继续：片 i 覆盖 [(i-1)*MAX_FILE_LEN, i*MAX_FILE_LEN)
    file.seek((msg_info->_seq - 1) * MAX_FILE_LEN);

    bool b_last = false;
    for (; MAX_CWND_SIZE - _cwnd_size > 0; ) {
        QByteArray buffer;
        //放入发送未回包集合
        msg_info->_flighting_seqs.insert(msg_info->_seq);
        //每次读取MAX_FILE_LEN字节发送
        buffer = file.read(MAX_FILE_LEN);
        if (buffer.isEmpty()) {
            break;
        }
        const int chunk_index = static_cast<int>(msg_info->_seq) - 1;
        if (chunk_index < 0 || chunk_index >= msg_info->_chunk_hashes.size()) {
            qWarning() << "[FileTcpMgr] chunk hash missing idx=" << chunk_index;
            break;
        }
        QJsonObject sendObj;
        sendObj["message_id"] = QString::number(msg_info->_msg_id);
        sendObj["offset"] = QString::number((msg_info->_seq - 1) * MAX_FILE_LEN);
        sendObj["chunk_sha256"] = msg_info->_chunk_hashes[chunk_index];
        sendObj["data"] = QString::fromLatin1(buffer.toBase64());

        b_last = (msg_info->_seq >= msg_info->_max_seq);
        msg_info->_seq++;
        msg_info->_current_size = qMin(msg_info->_seq * MAX_FILE_LEN, msg_info->_total_size);

        QJsonDocument doc(sendObj);
        auto send_data = doc.toJson(QJsonDocument::Compact);
        //直接发送，其实是放入tcpmgr发送队列
        SendData(ID_RESOURCE_CHUNK_UPLOAD_REQ, send_data);
        _cwnd_size++;
        if (b_last) {
            break;
        }
    }

    file.close();
}

void FileTcpMgr::PostBatchSend(std::shared_ptr<MsgInfo> msg_info) {
    emit sig_batch_send(msg_info);
}

void FileTcpMgr::slot_batch_send(std::shared_ptr<MsgInfo> msg_info) {
    BatchSend(msg_info);
}

void FileTcpMgr::slot_tcp_close() {
    //主动关闭：重置鉴权态，确保下次连接必须重新登录
    _authenticated = false;
    _socket.close();
}

void FileTcpMgr::slot_continue_upload_file(QString unique_name) {
    //续传统一入口：向服务端查询真实偏移（1509），1508/1510 对齐后 BatchSend 继续
    auto msg_info = UserMgr::GetInstance()->GetTransFileByName(unique_name);
    if (msg_info == nullptr || msg_info->_msg_id <= 0) {
        return;
    }
    msg_info->_transfer_state = TransferState::Uploading;
    QJsonObject req;
    req["message_id"] = QString::number(msg_info->_msg_id);
    SendData(ID_RESOURCE_UPLOAD_PROGRESS_REQ,
        QJsonDocument(req).toJson(QJsonDocument::Compact));
}

void FileTcpMgr::slot_continue_download_file(QString unique_name) {
    //续传统一入口：重走 1511 元数据（从本地 .part 大小续传）
    auto file_info = UserMgr::GetInstance()->GetTransFileByName(unique_name);
    if (file_info == nullptr || file_info->_msg_id <= 0) {
        return;
    }
    file_info->_transfer_state = TransferState::Downloading;
    slot_start_resource_download(file_info);
}

void FileTcpMgr::CloseConnection() {
    emit sig_close();
}

void FileTcpMgr::SendDownloadInfo(std::shared_ptr<DownloadInfo> download, QString req_type) {
    QJsonObject jsonObj;
    jsonObj["name"] = download->_name;
    jsonObj["seq"] = download->_seq;
    jsonObj["trans_size"] = 0;
    jsonObj["total_size"] = 0;
    //3.2 token/uid 由 Resource 从 session 派生，不再发送
    jsonObj["client_path"] = download->_client_path;
    jsonObj["req_type"] = req_type;
    QJsonDocument doc(jsonObj);
    auto send_data = doc.toJson();

    SendData(ID_DOWN_LOAD_FILE_REQ, send_data);
}


FileTcpThread::FileTcpThread()
{
    _file_tcp_thread = new QThread();
    FileTcpMgr::GetInstance()->moveToThread(_file_tcp_thread);
    QObject::connect(_file_tcp_thread, &QThread::finished, _file_tcp_thread, &QObject::deleteLater);

    _file_tcp_thread->start();
}

FileTcpThread::~FileTcpThread()
{
    _file_tcp_thread->quit();
    _file_tcp_thread->wait();
}
