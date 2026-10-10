#include "resourcecodec.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <cmath>
#include <limits>

namespace rc {

const qint64 kChunkSize = 32 * 1024;

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
    QJsonObject req;
    req["message_id"] = QString::number(message_id);
    req["offset"] = QString::number(offset);
    req["chunk_sha256"] = chunk_sha256;
    req["data"] = QString::fromLatin1(raw.toBase64());
    return QJsonDocument(req).toJson(QJsonDocument::Compact);
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
    if (out->error != 0) {
        ok = true;
    } else {
        //成功回包：server_offset/file_size_bytes/status/sha256 必需。
        ok = out->server_offset >= 0 && out->file_size_bytes >= 0
            && out->status >= 0 && IsValidSha256Hex(out->sha256);
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
    QJsonObject obj;
    bool ok = false;
    if (!ParseObject(payload, &out->message_id, &obj, &ok)) {
        return false;
    }
    if (!ReadError(obj, &out->error)) {
        return true;
    }
    out->offset = OptU64(obj, "offset", -1);
    out->bytes = OptU64(obj, "bytes", -1);
    out->chunk_sha256 = ReadString(obj, "chunk_sha256");
    const QString data_b64 = ReadString(obj, "data");
    if (!data_b64.isEmpty()) {
        out->data = QByteArray::fromBase64(data_b64.toLatin1());
    }
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

} // namespace rc
