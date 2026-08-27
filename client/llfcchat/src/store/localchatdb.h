#ifndef LOCALCHATDB_H
#define LOCALCHATDB_H

#include <QString>
#include <QList>
#include <QSqlDatabase>
#include <QJsonArray>
#include "localmessageDTO.h"

//outbox 操作类型
const QString OUTBOX_OP_SEND_TEXT = "SEND_TEXT";
const QString OUTBOX_OP_SEND_RESOURCE = "SEND_RESOURCE";

//消息发送状态
const QString SEND_STATE_SENDING = "sending";
const QString SEND_STATE_SENT = "sent";
const QString SEND_STATE_FAILED = "failed";

//资源 outbox 阶段（图片/文件统一）
const QString RESOURCE_STAGE_METADATA = "metadata";
const QString RESOURCE_STAGE_UPLOADING = "uploading";

//本地聊天 SQLite 同步核心（非 QObject）。
//供 LocalChatStore worker 线程与单元测试直接实例化；所有方法同步、线程归属由调用方保证。
class LocalChatDb {
public:
    //conn_name 为空时自动生成唯一连接名（单元测试可并行多实例）
    explicit LocalChatDb(const QString& conn_name = QString());
    ~LocalChatDb();

	//打开数据库：PRAGMA + 建 messages/conversations/outbox/sync_state/
	//friend_requests/contacts 六表
    bool open(const QString& dbPath, qint64 selfUid);
    void close();
    bool isOpen() const;

    //—— 发送链路（单事务）——
    //INSERT messages(sending) + INSERT outbox，回填 dto.local_id；
    //资源消息（type 1/3）要求 content_hash/mime_type 已由后台哈希填好
    bool enqueueSend(LocalMessageDTO& dto);
    //1302 到达：UPDATE messages(server_message_id/sent/chat_time) + 删 outbox
    bool confirmTextSent(const QString& clientMessageId, qint64 serverMessageId,
        const QString& chatTime, LocalMessageDTO* out);
    //1504 到达：回写 server_message_id + outbox.stage=uploading（不删 outbox）
    bool updateResourceStage(const QString& clientMessageId, qint64 serverMessageId,
        const QString& stage, LocalMessageDTO* out);
    //1508 上传完成（resource_status=Ready）：send_state=sent + 删 outbox
    bool confirmResourceSent(const QString& clientMessageId, LocalMessageDTO* out);
    //冲突/源文件丢失/资源终态：send_state=failed + 删 outbox
    bool markSendFailed(const QString& clientMessageId, LocalMessageDTO* out);
    bool getMessageByClientId(const QString& clientMessageId, LocalMessageDTO* out);

    //—— 接收与同步（单事务）——
    //INSERT OR IGNORE messages + UPSERT conversations(unread/preview/last_server_message_id)
	//insertedIds 返回实际插入的 server ids 供 UI 去重
    bool insertIncoming(const QList<LocalMessageDTO>& msgs,
		QList<qint64>* insertedIds = nullptr);
    //整页 + 推进 sync_state 游标，同一事务；失败整体回滚游标不动
    bool applySyncPage(const QList<LocalMessageDTO>& msgs, qint64 newSyncSeq,
        QList<qint64>* insertedIds = nullptr);
    //1403/1404 历史页回写：不产生 ACK、不计未读；更新 oldest_loaded_message_id/history_complete
    bool insertHistoryPage(qint64 threadId, const QList<LocalMessageDTO>& msgs,
        bool historyComplete);
    bool upsertConversations(const QList<LocalConversationDTO>& convs);
	bool applySnapshot(const QJsonArray& friendRequests, const QJsonArray& contacts,
		bool replaceCurrent);

    //—— sync_state（单行 id=1 UPSERT）——
	bool getSyncState(qint64* lastRecvSeq, bool* bootstrapComplete);
    bool markBootstrapComplete(qint64 checkpoint);

    //—— outbox ——
    bool loadOutbox(QList<OutboxEntryDTO>* entries);
    bool deleteOutboxEntry(const QString& dedupKey);
    bool updateOutboxRetry(const QString& dedupKey, int retryCount, qint64 nextRetryAt);

    //—— 查询 ——
    bool loadConversations(QList<LocalConversationDTO>* convs);
    bool loadRecentMessages(qint64 threadId, int limit, QList<LocalMessageDTO>* msgs);
    bool loadOlderMessages(qint64 threadId, qint64 beforeMessageId, int limit,
        QList<LocalMessageDTO>* msgs);

private:
    bool initSchema();
	bool applyUnifiedMessage(const LocalMessageDTO& dto, bool* inserted);
    //INSERT OR IGNORE 单条 messages，返回是否实际插入（供 insertedIds 判定）
    bool insertMessageIgnore(const LocalMessageDTO& dto, bool* inserted);
    //会话 UPSERT：incoming=true 时未读自增并更新 preview，否则只更新 last_server_message_id
    bool upsertConversationOnMessage(const LocalMessageDTO& dto, bool incoming);
    LocalMessageDTO readMessageRow(class QSqlQuery& query);
    bool fillMessageByClientId(const QString& clientMessageId, LocalMessageDTO* out);

    QSqlDatabase _db;
    QString _conn_name;
    qint64 _self_uid = 0;
};

#endif // LOCALCHATDB_H
