#ifndef TCPMGR_H
#define TCPMGR_H
#include <QTcpSocket>
#include "singleton.h"
#include "global.h"
#include <functional>
#include <QObject>
#include "userdata.h"
#include <QJsonArray>
#include <memory>
#include <QThread>
#include <QQueue>

class TcpThread:public std::enable_shared_from_this<TcpThread> {
public:
    TcpThread();
    ~TcpThread();
private:
    QThread* _tcp_thread;
};

//TcpMgr 回归纯传输：帧收发 + initHandlers 分发 + 公共 sendData 通道。
//重传与统一增量同步语义移交 OutboxDispatcher/ChatSyncManager。
class TcpMgr:public QObject, public Singleton<TcpMgr>,
        public std::enable_shared_from_this<TcpMgr>
{
    Q_OBJECT
public:
   ~ TcpMgr();
    void CloseConnection();
    void ReconnectChat(const QString& host, quint16 port);

private:
    friend class Singleton<TcpMgr>;
    TcpMgr();
    void registerMetaType();
    void initHandlers();
    // 1102 Chat 登录/重连回包
    void handleChatLoginRsp(ReqId id, int len, QByteArray data);
    // 1202 搜索用户回包
    void handleSearchUserRsp(ReqId id, int len, QByteArray data);
    // 1204 添加好友回包
    void handleAddFriendRsp(ReqId id, int len, QByteArray data);
    // 1208 认证好友回包
    void handleAuthFriendRsp(ReqId id, int len, QByteArray data);
	// 1701 唯一用户消息实时通知
	void handleUserMessageNotify(ReqId id, int len, QByteArray data);
    // 1302 文本消息发送回包
    void handleTextChatMsgRsp(ReqId id, int len, QByteArray data);
    // 1105 被踢下线通知
    void handleNotifyOfflineReq(ReqId id, int len, QByteArray data);
    // 1104 心跳回包
    void handleHeartbeatRsp(ReqId id, int len, QByteArray data);
    // 1402 会话列表回包
    void handleLoadChatThreadRsp(ReqId id, int len, QByteArray data);
    // 1306 创建私聊回包
    void handleCreatePrivateChatRsp(ReqId id, int len, QByteArray data);
    // 1404 历史消息回包
    void handleLoadChatMsgRsp(ReqId id, int len, QByteArray data);
    // 1504 资源消息创建回包
    void handleCreateResourceMsgRsp(ReqId id, int len, QByteArray data);
    // 1406 增量同步回包
    void handleSyncMessageRsp(ReqId id, int len, QByteArray data);
    void handleMsg(ReqId id, int len, QByteArray data);
    void finishReconnectFailure();
    void CreatePlaceholderResourceMsgL(QString cache_dir, QString msg_content,
        qint64 msg_id, qint64 thread_id, int send_uid, int recv_id, int status, QString chat_time,
        ChatMsgType msg_type, std::vector<std::shared_ptr<ChatDataBase>>& chat_datas);
    QTcpSocket _socket;
    QString _host;
    uint16_t _port;
    std::shared_ptr<ServerInfo> _server_info;  //登录流程保存，Chat 认证后传给 FileTcpMgr
    QByteArray _buffer;
    bool _b_recv_pending;
    quint16 _message_type;
    quint16 _message_len;
    typedef void (TcpMgr::*ChatHandler)(ReqId id, int len, QByteArray data);
    QMap<ReqId, ChatHandler> _handlers;
    //发送队列
    QQueue<QByteArray> _send_queue;
    //正在发送的包
    QByteArray  _current_block;
    //当前已发送的字节数
    qint64        _bytes_sent;
    //是否正在发送
    bool _pending;
    bool _manual_close;
    bool _reconnecting;
    bool _disconnect_notified;
public slots:
    void slot_tcp_close();
    void slot_tcp_connect(std::shared_ptr<ServerInfo> si);
    void slot_reconnect_chat(QString host, quint16 port);
    void slot_send_data(ReqId reqId, QByteArray data);
signals:
    void sig_close();
    void sig_con_success(bool bsuccess);
    void sig_reconnect_chat(QString host, quint16 port);
    void sig_reconnect_finished(bool success);
    void sig_send_data(ReqId reqId, QByteArray data);
    //1102 登录/重连成功：OutboxDispatcher/ChatSyncManager 启动点
    void sig_chat_login_ready();
	void sig_login_snapshot(QJsonObject snapshot, bool initialLogin);
    void sig_swich_chatdlg();
    //3.2 Chat 认证成功后触发 FileTcpMgr 连接 Resource（携带 ServerInfo）
    void sig_connect_resource(std::shared_ptr<ServerInfo> si);
    void sig_load_apply_list(QJsonArray json_array);
    void sig_login_failed(int);
    void sig_user_search(std::shared_ptr<SearchInfo>);
    void sig_friend_apply(std::shared_ptr<AddFriendApply>);
	void sig_friend_request_handled(qint64 messageId, int businessStatus);
    void sig_add_auth_friend(std::shared_ptr<AuthInfo>);
    void sig_auth_rsp(std::shared_ptr<AuthRsp>);
    void sig_text_chat_msg(std::shared_ptr<TextChatData> msg);
    void sig_notify_offline();
    void sig_connection_closed();
    void sig_load_chat_thread(bool load_more, qint64 last_thread_id,
        std::vector<std::shared_ptr<ChatThreadInfo>> chat_list);
    void sig_create_private_chat(int uid, int other_id, qint64 thread_id);
    void sig_load_chat_msg(qint64 thread_id, qint64 message_id, bool load_more,
        std::vector<std::shared_ptr<ChatDataBase>> msg_list);
    void sig_img_chat_msg(std::shared_ptr<ImgChatData> msg_list);
    //文件消息（复用 ImgChatData 载荷；不自动下载，等用户点击）
    void sig_file_chat_msg(std::shared_ptr<ImgChatData> msg_list);
    //—— 领域转发信号：1302/1504 → OutboxDispatcher，1701/1406 → ChatSyncManager ——
    //1302 文本回包（含 MESSAGE_CONFLICT/transient，由 Dispatcher 判定）
    void sig_text_msg_rsp_forward(int error, QString unique_id, qint64 message_id,
        QString chat_time);
    //1504 资源消息创建回包（含 MESSAGE_CONFLICT/RESOURCE_*/transient，由 Dispatcher 判定）
    void sig_resource_msg_meta_rsp_forward(int error, QString unique_id, QString file_name,
        qint64 message_id, qint64 thread_id, qint64 fromuid, qint64 touid);
	void sig_user_message_notify(QJsonObject envelope);
	void sig_user_message_business_rsp(QJsonObject envelope);
    //1406 增量同步回包（原始 JSON 对象交 ChatSyncManager）
    void sig_sync_message_rsp(QJsonObject rsp);
};

#endif // TCPMGR_H
