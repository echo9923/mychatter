#ifndef LOCALMESSAGEDTO_H
#define LOCALMESSAGEDTO_H

#include <QString>
#include <QList>
#include <QMetaType>

//本地消息值对象（跨线程传递，仅含值类型字段，禁止携带裸指针）
struct LocalMessageDTO {
    qint64 local_id = 0;            //messages 表自增主键（0=未入库）
    qint64 server_message_id = 0;   //服务端 message_id（0=未确认/NULL 语义）
    QString client_message_id;      //客户端幂等键（QUuid 串）
    qint64 thread_id = 0;
    qint64 sender_id = 0;
    qint64 receiver_id = 0;
    int message_type = 0;           //0 文本 1 图片 3 文件（对齐 ChatMsgType）
    QString content;                //文本内容 / 资源原始文件名（仅展示）
    QString local_path;             //资源本地路径（发送端源文件/接收端缓存）
    QString content_size;           //十进制字符串，维持线协议现状
    int resource_status = 0;        //资源生命周期（0 待上传/1 就绪/2 失败过期；文本为 0）
    QString content_hash;           //整文件 SHA-256（资源消息）
    QString mime_type;              //资源 MIME 类型（资源消息）
    QString send_state;             //sending/sent/failed
    QString created_at;             //chat_time 字符串
};

//本地会话值对象
struct LocalConversationDTO {
    qint64 thread_id = 0;
    qint64 peer_uid = 0;                    //私聊对方 uid
    qint64 last_server_message_id = 0;      //最后一条服务端 message_id
    QString last_message_preview;
    int unread_count = 0;
    qint64 oldest_loaded_message_id = 0;    //本地已加载的最旧 message_id（1403 游标）
    bool history_complete = false;          //历史是否已全部拉取
    QString updated_at;
};

//outbox 可靠重试条目
struct OutboxEntryDTO {
    qint64 operation_id = 0;        //outbox 表自增主键
    QString operation_type;         //SEND_TEXT/SEND_RESOURCE/DELIVERY_ACK
    QString dedup_key;              //去重键（唯一约束）
    QString request_id;             //关联 client_message_id（ACK 条目为空）
    QString payload;                //原始请求 JSON（ACK 条目为 {"message_id":"..."}）
    QString stage;                  //资源阶段：metadata/uploading，其余为空
    int retry_count = 0;
    qint64 next_retry_at = 0;       //下次重试时刻（epoch ms）
};

Q_DECLARE_METATYPE(LocalMessageDTO)
Q_DECLARE_METATYPE(LocalConversationDTO)
Q_DECLARE_METATYPE(OutboxEntryDTO)
Q_DECLARE_METATYPE(QList<LocalMessageDTO>)
Q_DECLARE_METATYPE(QList<LocalConversationDTO>)
Q_DECLARE_METATYPE(QList<OutboxEntryDTO>)

#endif // LOCALMESSAGEDTO_H
