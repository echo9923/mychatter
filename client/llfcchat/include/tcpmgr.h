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
#include <QTimer>
#include <QDateTime>

class TcpThread:public std::enable_shared_from_this<TcpThread> {
public:
    TcpThread();
    ~TcpThread();
private:
    QThread* _tcp_thread;
};

//持久化待重传请求（§6.1）
struct PendingRequest {
    ReqId id;                  // 1017 文本 / 1035 图片
    QByteArray payload;        // 原始 JSON payload
    QStringList unique_ids;    // 本批次客户端唯一标识
    qint64 retry_delay_ms;     // 当前退避间隔（倍增上限 30s）
    qint64 next_send_epoch_ms; // 下次发送时刻（epoch ms）
};

class TcpMgr:public QObject, public Singleton<TcpMgr>,
        public std::enable_shared_from_this<TcpMgr>
{
    Q_OBJECT
public:
   ~ TcpMgr();
    void CloseConnection();
    void SendData(ReqId reqId, QByteArray data);
    //可靠发送：payload + unique_ids 进入持久 pending，按退避无限重传直到 1018/1036 或冲突（§6.1）
    void SendReliableChat(ReqId id, QByteArray payload, const QStringList& unique_ids);
    //§6.2 纠错：仅 emit queued signal，TCP 线程 slot 在 GUI thread models 建好后执行 rebuild+重发
    void StartPendingReplay();

private:
    friend class Singleton<TcpMgr>;
    TcpMgr();
    void registerMetaType();
    void initHandlers();
    void handleMsg(ReqId id, int len, QByteArray data);
    void CreatePlaceholderImgMsgL(QString img_path_str, QString msg_content,
        int msg_id, int thread_id, int send_uid, int recv_id, int status, QString chat_time,
        std::vector<std::shared_ptr<ChatDataBase>>& chat_datas);
    QTcpSocket _socket;
    QString _host;
    uint16_t _port;
    QByteArray _buffer;
    bool _b_recv_pending;
    quint16 _message_id;
    quint16 _message_len;
    QMap<ReqId, std::function<void(ReqId id, int len, QByteArray data)>> _handlers;
    //发送队列
    QQueue<QByteArray> _send_queue;
    //正在发送的包
    QByteArray  _current_block;
    //当前已发送的字节数
    qint64        _bytes_sent;
    //是否正在发送
    bool _pending;
    //—— 可靠重传状态（全部只在 TCP 线程访问）——
    QTimer* _retry_timer;                 // 250ms 扫描定时器，parent 到 this
    QList<PendingRequest> _pending_requests;
    int _delivery_uid;                    // 当前加载 pending 的 uid（0=未加载）
    qint64 _retry_initial_ms;             // 初始退避（配置 RequestRetryInitialMs）
    qint64 _retry_max_ms;                 // 退避上限（配置 RequestRetryMaxMs）
    //可靠重传内部方法（均在 TCP 线程执行）
    void loadDeliveryConfig();
    void addPendingRequest(ReqId id, QByteArray payload, QStringList unique_ids);
    void persistPendingRequests();
    void restorePendingRequests(int uid);
    void loadPendingFromDisk(int uid);
    void removePendingByUniqueId(const QString& unique_id);
    void handleTextConflict(int thread_id, int fromuid, const QStringList& conflict_ids);
    void handleImageConflict(const QString& conflict_id);
    bool rebuildImagePending(PendingRequest& req);
    void rebuildTextPending(const PendingRequest& req);
public slots:
    void slot_tcp_close();
    void slot_tcp_connect(std::shared_ptr<ServerInfo> si);
    void slot_send_data(ReqId reqId, QByteArray data);
    void slot_send_reliable_chat(ReqId id, QByteArray payload, QStringList unique_ids);
    void slot_retry_timeout();
    void slot_start_pending_replay();
    void slot_test() {
        qDebug() << "receve thread is " << QThread::currentThread();
        qDebug() << "slot test......";
    }
signals:
    void sig_close();
    void sig_con_success(bool bsuccess);
    void sig_send_data(ReqId reqId, QByteArray data);
    //线程边界信号：公有 API 只 emit 此信号，slot_send_reliable_chat 在 TCP 线程执行
    void sig_send_reliable_chat(ReqId id, QByteArray payload, QStringList unique_ids);
    //§6.2：公有 StartPendingReplay 只 emit 此信号，slot_start_pending_replay 在 TCP 线程执行
    void sig_start_pending_replay();
    void sig_swich_chatdlg();
    void sig_load_apply_list(QJsonArray json_array);
    void sig_login_failed(int);
    void sig_user_search(std::shared_ptr<SearchInfo>);
    void sig_friend_apply(std::shared_ptr<AddFriendApply>);
    void sig_add_auth_friend(std::shared_ptr<AuthInfo>);
    void sig_auth_rsp(std::shared_ptr<AuthRsp>);
    void sig_text_chat_msg(std::vector<std::shared_ptr<TextChatData>> msg_list);
    void sig_notify_offline();
    void sig_connection_closed();
    void sig_load_chat_thread(bool load_more, int last_thread_id, 
        std::vector<std::shared_ptr<ChatThreadInfo>> chat_list);
    void sig_create_private_chat(int uid, int other_id, int thread_id);
    void sig_load_chat_msg(int thread_id, int message_id, bool load_more,
        std::vector<std::shared_ptr<ChatDataBase>> msg_list);

    void sig_chat_msg_rsp(int thread_id, std::vector<std::shared_ptr<TextChatData>> msg_list);
    void sig_chat_img_rsp(int thread_id, std::shared_ptr<ImgChatData> msg_list);
    void sig_img_chat_msg(std::shared_ptr<ImgChatData> msg_list);
};

#endif // TCPMGR_H
