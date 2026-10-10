#ifndef RESOURCECODEC_H
#define RESOURCECODEC_H

#include <QByteArray>
#include <QString>

//资源域帧编解码（P1c 线协议 v2）。1505/1512 为二进制帧
//[u16 meta_len 大端][meta JSON][raw]（raw=分片原始字节，proto/resource_wire.h）；
//其余（1507/1508/1509/1510）仍为 JSON。64 位字段（message_id/offset/
//server_offset/file_size_bytes/bytes/total_size/event_seq）线上为十进制字符串，
//解析端严格只收字符串（拒绝数字字面量/符号/空白）；无 Base64、无 data 字段。

namespace rc {

//wire 分片单位唯一真值：引用 proto/resource_wire.h 的 256KiB（不可配置）。
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
    //P2 秒传挑战（mode=="proof" 时有效；普通上传为空/-1）
    QString mode;               //"proof" 或空（普通上传）
    QString challenge_nonce;    //32 字符 hex（16 字节 nonce）
    qint64 challenge_offset = -1;
    qint64 challenge_length = -1;
};

//P2 1514 秒传证明回包：{error, message_id, status, server_offset, event_seq}
//ok 语义（见 DecodeInstantProofRsp）：error!=0 时 ok=true（业务字段可缺失，
//按 error 分类处理）；error==0（成功）时 status/server_offset 必须存在且
//**event_seq 必须 >0**——1514 成功必为"已发布"，发布事务为接收者分配
//event_seq，缺失/0/非十进制字符串都是坏包，调用方按 ERR_JSON 计数处理
//（有界：连续坏包达 json_error_limit 转永久失败，不无限重同步）。
//status 的语义判定（必须 == Published）在 engine 侧，与 1508 一致。
struct InstantProofRsp {
    bool ok = false;
    int error = 0;
    qint64 message_id = 0;
    int status = -1;
    qint64 server_offset = -1;
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

//1512 分片下载回包（二进制帧）：meta={error, message_id, offset, bytes,
//chunk_sha256, total_size, is_last}，raw=分片原始字节（data 字段已废除）
struct ChunkDownRsp {
    bool ok = false;
    int error = 0;
    qint64 message_id = 0;
    qint64 offset = -1;
    qint64 bytes = -1;          //声明数据长度（解码后字节数）
    QString chunk_sha256;       //小写 hex
    QByteArray data;            //帧 raw 部分的原始分片字节（无 Base64）
    qint64 total_size = -1;
    bool is_last = false;
};

//1505（二进制帧）：meta={message_id, offset, chunk_sha256}，raw=分片原始字节
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

//P2 1513：{message_id, proof}（proof=hex(SHA256(nonce_raw16||file[off,off+len)))）
QByteArray EncodeInstantProofReq(qint64 message_id, const QString& proof_hex);
bool DecodeInstantProofRsp(const QByteArray& payload, InstantProofRsp* out);

} // namespace rc

#endif // RESOURCECODEC_H
