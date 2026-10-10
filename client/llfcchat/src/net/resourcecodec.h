#ifndef RESOURCECODEC_H
#define RESOURCECODEC_H

#include <QByteArray>
#include <QString>

//资源域帧编解码（P1b-01）。当前仍为 JSON+Base64 旧线协议；P1c 换二进制帧时
//只改本文件，引擎与传输层接口不变。64 位字段（message_id/offset/server_offset/
//file_size_bytes/bytes/total_size/event_seq）线上为十进制字符串，解析端兼容
//数字字面量（服务端各回包均为字符串）。

namespace rc {

//与服务端 kResourceUploadChunkSize（=MAX_FILE_LEN=32KiB）对齐的分片单位；
//P1c 随线协议 v2 迁移到 proto/resource_wire.h。
extern const qint64 kChunkSize;

//1506 上传分片回包：{error, message_id, offset, server_offset, status, event_seq?}
struct ChunkUploadRsp {
    bool ok = false;            //JSON 可解析且必带字段类型正确
    int error = 0;
    qint64 message_id = 0;
    qint64 offset = -1;         //回显请求 offset（在途队首比对用）
    qint64 server_offset = -1;  //-1=缺失
    int status = -1;            //chat_messages.status：0 Pending/1 Published/2 Failed
    qint64 event_seq = 0;
};

//1508 上传进度回包：{error, message_id, server_offset, file_size_bytes, status,
//sha256, chunk_size, event_seq?}
struct ProgressRsp {
    bool ok = false;
    int error = 0;
    qint64 message_id = 0;
    qint64 server_offset = -1;
    qint64 file_size_bytes = -1;
    int status = -1;
    QString sha256;
    qint64 chunk_size = -1;     //服务端分片对齐单位
    qint64 event_seq = 0;
};

//1510 下载信息回包：{error, message_id, original_file_name, file_size_bytes,
//sha256, mime_type, message_type, status}
struct DownInfoRsp {
    bool ok = false;
    int error = 0;
    qint64 message_id = 0;
    QString original_file_name;
    qint64 file_size_bytes = -1;
    QString sha256;
    QString mime_type;
    int message_type = 0;
    int status = -1;
};

//1512 分片下载回包：{error, message_id, offset, bytes, chunk_sha256?, data?,
//total_size, is_last}
struct ChunkDownRsp {
    bool ok = false;
    int error = 0;
    qint64 message_id = 0;
    qint64 offset = -1;
    qint64 bytes = -1;          //声明数据长度（解码后字节数）
    QString chunk_sha256;       //小写 hex
    QByteArray data;            //已 Base64 解码的原始分片
    qint64 total_size = -1;
    bool is_last = false;
};

//1505：{message_id, offset, chunk_sha256, data(Base64)}
QByteArray EncodeChunkUploadReq(qint64 message_id, qint64 offset,
    const QString& chunk_sha256, const QByteArray& raw);
bool DecodeChunkUploadRsp(const QByteArray& payload, ChunkUploadRsp* out);

//1507：{message_id}
QByteArray EncodeProgressReq(qint64 message_id);
bool DecodeProgressRsp(const QByteArray& payload, ProgressRsp* out);

//1509：{message_id}
QByteArray EncodeDownInfoReq(qint64 message_id);
bool DecodeDownInfoRsp(const QByteArray& payload, DownInfoRsp* out);

//1511：{message_id, offset}
QByteArray EncodeChunkDownReq(qint64 message_id, qint64 offset);
bool DecodeChunkDownRsp(const QByteArray& payload, ChunkDownRsp* out);

} // namespace rc

#endif // RESOURCECODEC_H
