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

//§6.5 recipient ACK 重传状态（每个已收到但服务端未确认的 message_id）
struct AckPending {
    qint64 retry_delay_ms;     // 当前退避间隔（倍增上限同 sender）
    qint64 next_send_epoch_ms; // 下次发送时刻（epoch ms）
};

//§6.2 纠错：跨线程 replay DTO（TCP 线程解析内存 pending 构造，queued 传到 GUI 线程）
//禁止携带裸指针；仅含值类型字段。文本 pending 摘要
struct TextReplayDTO {
    int thread_id;
    int fromuid;
    QStringList unique_ids;   // 仍 pending 的 unique_id（与 contents 一一对应）
    QStringList contents;     // 对应文本内容
};
//图片 pending 摘要
struct ImageReplayDTO {
    int thread_id;
    int fromuid;              // sender
    int touid;                // receiver
    QString name;             // 文件唯一名（UserMgr::AddTransFile key）
    QString md5;
    qint64 content_size;
    QString text_or_url;      // 本地文件路径
    QString unique_id;
};

Q_DECLARE_METATYPE(TextReplayDTO)
Q_DECLARE_METATYPE(ImageReplayDTO)
Q_DECLARE_METATYPE(std::vector<TextReplayDTO>)
Q_DECLARE_METATYPE(std::vector<ImageReplayDTO>)

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
    //§6.2 纠错：仅 emit queued signal，TCP 线程 slot 解析 pending→DTO→emit 给 GUI 线程重建
    void StartPendingReplay();
    //§6.6：仅 emit queued signal，TCP 线程 slot 启动离线 pull 循环
    void StartOfflinePull();

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
    //—— §6.5 recipient ACK 状态（全部只在 TCP 线程访问）——
    QSet<int> _pending_ui_ack;            // 已收到、待 ChatDialog 确认 UI 插入的 message_id
    QMap<int, AckPending> _pending_ack;   // 已确认 UI、待服务端 1050 回复的 message_id
    qint64 _ack_retry_initial_ms;         // ACK 初始退避（配置 AckRetryInitialMs）
    //—— §6.6 离线 pull 状态（全部只在 TCP 线程访问）——
    QTimer* _offline_pull_timer;          // 定时拉取定时器，parent 到 this
    int _offline_pull_interval_ms;        // 配置 OfflinePullIntervalMs
    int _offline_pull_batch;              // 配置 OfflinePullBatch
    //可靠重传内部方法（均在 TCP 线程执行）
    void loadDeliveryConfig();
    void addPendingRequest(ReqId id, QByteArray payload, QStringList unique_ids);
    void persistPendingRequests();
    void restorePendingRequests(int uid);
    void loadPendingFromDisk(int uid);
    void removePendingByUniqueId(const QString& unique_id);
    void handleTextConflict(int thread_id, int fromuid, const QStringList& conflict_ids);
    void handleImageConflict(const QString& conflict_id);
    //§6.4 统一 envelope 分发（1019/1039/1052 共用）：文本走 sig_text_chat_msg，图片走 sig_img_chat_msg+下载
    void dispatchIncomingMessage(int message_id, const QString& unique_id,
        int thread_id, int fromuid, int touid, int msg_type,
        const QString& content, qint64 content_size,
        const QString& chat_time, int status);
    //§6.5 ACK 批量发送（取出到期项，发 1049，更新退避，持久化）
    void flushPendingAcks();
    void persistAckPending();
    void loadAckPendingFromDisk(int uid);
public slots:
    void slot_tcp_close();
    void slot_tcp_connect(std::shared_ptr<ServerInfo> si);
    void slot_send_data(ReqId reqId, QByteArray data);
    void slot_send_reliable_chat(ReqId id, QByteArray payload, QStringList unique_ids);
    void slot_retry_timeout();
    void slot_start_pending_replay();
    void slot_replay_done(QStringList failed_unique_ids);
    void slot_start_offline_pull();
    void slot_offline_pull_timeout();
    void slot_msg_processed(int message_id);
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
    //§6.2 纠错：TCP 线程解析 pending → DTO，queued 到 GUI 线程重建 bubble/MsgInfo/QPixmap
    void sig_replay_pending(std::vector<TextReplayDTO> texts, std::vector<ImageReplayDTO> images);
    //§6.2 纠错：GUI 重建完成后回执，queued 回 TCP 线程（failed_unique_ids=文件缺失需删除的项）
    void sig_replay_result(QStringList failed_unique_ids);
    //§6.6：公有 StartOfflinePull 只 emit 此信号，slot_start_offline_pull 在 TCP 线程执行
    void sig_start_offline_pull();
    //§6.5：ChatDialog 插入/duplicate 后 emit，queued 回 TCP 线程触发 1049 ACK
    void sig_chat_msg_processed(int message_id);
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
