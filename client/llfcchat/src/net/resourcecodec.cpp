#include "resourcecodec.h"

#include <QDebug> //qWarning 流完整依赖（P1c 收尾：显式 include）
#include <QJsonDocument>
#include "resource_wire.h"
#include <QJsonObject>
#include <QJsonValue>

#include <cmath>
#include <limits>

namespace rc {

//P1c：与 proto/resource_wire.h 共享（v2 二进制分片 256KiB）
const qint64 kChunkSize = static_cast<qint64>(llfc_wire::kResourceChunkSize);

namespace {

//64 位字段严格解析：线上契约唯一形态是十进制字符串（AGENTS.md）。
//非空、纯 ASCII 数字（无符号/空白/小数点）、值 ≤ qint64 max；
//数字字面量（JSON double）一律拒绝——double 无法无损承载 64 位。
bool StrictU64(const QJsonValue& v, qint64* out)
{
    if (!v.isString()) {
        return false;
    }
    const QString s = v.toString();
    if (s.isEmpty()) {
        return false;
    }
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (c < QLatin1Char('0') || c > QLatin1Char('9')) {
            return false;
        }
    }
    bool ok = false;
    const qulonglong u = s.toULongLong(&ok);
    if (!ok || u > static_cast<qulonglong>(std::numeric_limits<qint64>::max())) {
        return false;
    }
    *out = static_cast<qint64>(u);
    return true;
}

//32 位字段（error/status/message_type）：JSON 数字、有限、整值、int 范围。
bool StrictI32(const QJsonValue& v, int* out)
{
    if (!v.isDouble()) {
        return false;
    }
    const double d = v.toDouble();
    if (!(d == d) || std::floor(d) != d || d < -2147483648.0
        || d > 2147483647.0) {
        return false;
    }
    *out = static_cast<int>(d);
    return true;
}

//可选 64 位字段：缺失/不合法返回 dflt（不使回包整体失效）。
qint64 OptU64(const QJsonObject& obj, const char* key, qint64 dflt)
{
    qint64 v = 0;
    return StrictU64(obj.value(QLatin1String(key)), &v) ? v : dflt;
}

qint64 OptI32(const QJsonObject& obj, const char* key, qint64 dflt)
{
    int v = 0;
    return StrictI32(obj.value(QLatin1String(key)), &v) ? static_cast<qint64>(v)
                                                        : dflt;
}

//严格 SHA-256 hex：恰好 64 个 [0-9a-f]（小写）。成功回包的哈希字段
//（1508/1510 sha256、1512 chunk_sha256）必须满足，空串/大写/非 hex 均非法。
bool IsValidSha256Hex(const QString& s)
{
    if (s.size() != 64) {
        return false;
    }
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9'))
                || (c >= QLatin1Char('a') && c <= QLatin1Char('f')))) {
            return false;
        }
    }
    return true;
}

QString ReadString(const QJsonObject& obj, const char* key)
{
    const QJsonValue v = obj.value(QLatin1String(key));
    return v.isString() ? v.toString() : QString();
}

//解析回包公共前置：合法 JSON 对象且 message_id 严格可读。
//返回 false=连传输都无法定位；*ok=false=可定位但回包不可用（error 缺失或
//成功回包缺必需字段），调用方按坏回包（2001 类）处理。
bool ParseObject(const QByteArray& payload, qint64* message_id, QJsonObject* out,
    bool* ok)
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
    *ok = false;
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }
    *out = doc.object();
    return StrictU64(out->value(QLatin1String("message_id")), message_id);
}

//error 为必需字段：缺失/非法即不可用回包（不得默认成功）。
bool ReadError(const QJsonObject& obj, int* error)
{
    return StrictI32(obj.value(QLatin1String("error")), error);
}

} // namespace

QByteArray EncodeChunkUploadReq(qint64 message_id, qint64 offset,
    const QString& chunk_sha256, const QByteArray& raw)
{
    //P1c：1505 二进制帧 [u16 meta_len BE][meta JSON][raw]（raw 不再 Base64）
    QJsonObject req;
    req["message_id"] = QString::number(message_id);
    req["offset"] = QString::number(offset);
    req["chunk_sha256"] = chunk_sha256;
    const QByteArray meta = QJsonDocument(req).toJson(QJsonDocument::Compact);
    const std::string meta_str(meta.constData(), static_cast<std::size_t>(meta.size()));
    const std::string raw_str(raw.constData(), static_cast<std::size_t>(raw.size()));
    std::string frame;
    if (!llfc_wire::EncodeFrame(meta_str, raw_str, &frame)) {
        qWarning() << "[rc] 1505 meta overflow, encode failed";
        return QByteArray();
    }
    return QByteArray(frame.data(), static_cast<int>(frame.size()));
}

bool DecodeChunkUploadRsp(const QByteArray& payload, ChunkUploadRsp* out)
{
    QJsonObject obj;
    bool ok = false;
    if (!ParseObject(payload, &out->message_id, &obj, &ok)) {
        return false;
    }
    if (!ReadError(obj, &out->error)) {
        return true; //ok=false：可定位但不可用
    }
    out->offset = OptU64(obj, "offset", -1);
    out->server_offset = OptU64(obj, "server_offset", -1);
    out->status = OptI32(obj, "status", -1);
    out->event_seq = OptU64(obj, "event_seq", 0);
    if (out->error != 0) {
        ok = true; //错误回包允许业务字段缺失
    } else {
        //成功回包：offset（队首比对）/server_offset/status 必需。
        ok = out->offset >= 0 && out->server_offset >= 0 && out->status >= 0;
    }
    out->ok = ok;
    return true;
}

QByteArray EncodeProgressReq(qint64 message_id)
{
    QJsonObject req;
    req["message_id"] = QString::number(message_id);
    return QJsonDocument(req).toJson(QJsonDocument::Compact);
}

bool DecodeProgressRsp(const QByteArray& payload, ProgressRsp* out)
{
    QJsonObject obj;
    bool ok = false;
    if (!ParseObject(payload, &out->message_id, &obj, &ok)) {
        return false;
    }
    if (!ReadError(obj, &out->error)) {
        return true;
    }
    out->server_offset = OptU64(obj, "server_offset", -1);
    out->file_size_bytes = OptU64(obj, "file_size_bytes", -1);
    out->status = OptI32(obj, "status", -1);
    out->sha256 = ReadString(obj, "sha256");
    out->chunk_size = OptU64(obj, "chunk_size", -1);
    out->event_seq = OptU64(obj, "event_seq", 0);
    //P2 秒传挑战（可选；普通上传无 mode/challenge）
    out->mode = ReadString(obj, "mode");
    const QJsonValue challenge = obj.value(QLatin1String("challenge"));
    if (challenge.isObject()) {
        const QJsonObject ch = challenge.toObject();
        out->challenge_nonce = ReadString(ch, "nonce");
        out->challenge_offset = OptU64(ch, "offset", -1);
        out->challenge_length = OptU64(ch, "length", -1);
    }
    if (out->error != 0) {
        ok = true;
    } else {
        //成功回包：server_offset/file_size_bytes/status/sha256 必需；
        //proof 模式额外要求 nonce/offset/length 齐备（严格 32 hex nonce）
        ok = out->server_offset >= 0 && out->file_size_bytes >= 0
            && out->status >= 0 && IsValidSha256Hex(out->sha256);
        if (ok && out->mode == QStringLiteral("proof")) {
            ok = out->challenge_nonce.size() == 32
                && out->challenge_offset >= 0 && out->challenge_length > 0;
        }
    }
    out->ok = ok;
    return true;
}

QByteArray EncodeDownInfoReq(qint64 message_id)
{
    QJsonObject req;
    req["message_id"] = QString::number(message_id);
    return QJsonDocument(req).toJson(QJsonDocument::Compact);
}

bool DecodeDownInfoRsp(const QByteArray& payload, DownInfoRsp* out)
{
    QJsonObject obj;
    bool ok = false;
    if (!ParseObject(payload, &out->message_id, &obj, &ok)) {
        return false;
    }
    if (!ReadError(obj, &out->error)) {
        return true;
    }
    out->original_file_name = ReadString(obj, "original_file_name");
    out->file_size_bytes = OptU64(obj, "file_size_bytes", -1);
    out->sha256 = ReadString(obj, "sha256");
    out->mime_type = ReadString(obj, "mime_type");
    out->message_type = OptI32(obj, "message_type", 0);
    out->status = OptI32(obj, "status", -1);
    if (out->error != 0) {
        ok = true;
    } else {
        //成功回包：file_size_bytes/sha256/status 必需（引擎消费）。
        ok = out->file_size_bytes >= 0 && out->status >= 0
            && IsValidSha256Hex(out->sha256);
    }
    out->ok = ok;
    return true;
}

QByteArray EncodeChunkDownReq(qint64 message_id, qint64 offset)
{
    QJsonObject req;
    req["message_id"] = QString::number(message_id);
    req["offset"] = QString::number(offset);
    return QJsonDocument(req).toJson(QJsonDocument::Compact);
}

bool DecodeChunkDownRsp(const QByteArray& payload, ChunkDownRsp* out)
{
    //P1c：1512 二进制帧 [u16 meta_len BE][meta JSON][raw]；raw 为分片字节
    std::string meta_str;
    std::string raw_str;
    const std::string frame(payload.constData(),
        static_cast<std::size_t>(payload.size()));
    if (!llfc_wire::DecodeFrame(frame, &meta_str, &raw_str)) {
        return false; //连 message_id 都不可读，无法定位传输
    }
    if (!raw_str.empty()) {
        out->data = QByteArray(raw_str.data(), static_cast<int>(raw_str.size()));
    }
    const QByteArray meta(meta_str.data(), static_cast<int>(meta_str.size()));
    QJsonObject obj;
    bool ok = false;
    if (!ParseObject(meta, &out->message_id, &obj, &ok)) {
        return false;
    }
    if (!ReadError(obj, &out->error)) {
        return true;
    }
    out->offset = OptU64(obj, "offset", -1);
    out->bytes = OptU64(obj, "bytes", -1);
    out->chunk_sha256 = ReadString(obj, "chunk_sha256");
    //data 已由帧 raw 部分承载（不再 Base64/JSON 字段）
    out->total_size = OptU64(obj, "total_size", -1);
    out->is_last = obj.value(QLatin1String("is_last")).toBool();
    if (out->error != 0) {
        ok = true;
    } else {
        //成功回包：非空片 offset/bytes/chunk_sha256/data/total_size 必需
        //（哈希须为合法 64 位小写 hex）。
        ok = out->offset >= 0 && out->bytes > 0 && !out->data.isEmpty()
            && out->total_size >= 0 && IsValidSha256Hex(out->chunk_sha256);
    }
    out->ok = ok;
    return true;
}

QByteArray EncodeInstantProofReq(qint64 message_id, const QString& proof_hex)
{
    QJsonObject req;
    req["message_id"] = QString::number(message_id);
    req["proof"] = proof_hex;
    return QJsonDocument(req).toJson(QJsonDocument::Compact);
}

bool DecodeInstantProofRsp(const QByteArray& payload, InstantProofRsp* out)
{
    QJsonObject obj;
    bool ok = false;
    if (!ParseObject(payload, &out->message_id, &obj, &ok)) {
        return false;
    }
    if (!ReadError(obj, &out->error)) {
        return true; //ok=false：可定位但不可用
    }
    out->status = OptI32(obj, "status", -1);
    out->server_offset = OptU64(obj, "server_offset", -1);
    out->event_seq = OptU64(obj, "event_seq", 0);
    if (out->error != 0) {
        ok = true; //错误回包（2118/2002 等）允许业务字段缺失
    } else {
        //成功回包：status/server_offset 必需，且 **event_seq 必须 > 0**
        //——1514 成功必为已发布，服务端发布事务一定分配 event_seq；缺失、
        //非十进制字符串或被写成 0 都是坏包，ok=false 让 engine 按 ERR_JSON
        //计数（连续坏包达上限转永久失败），绝不当作正常状态切换。
        //status 是否 == Published 由 engine 判定（本层只管字段存在性）。
        ok = out->status >= 0 && out->server_offset >= 0
            && out->event_seq > 0;
    }
    out->ok = ok;
    return true;
}

} // namespace rc
