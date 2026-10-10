#pragma once
// =============================================================================
// P1c 资源线协议 v2：二进制分片帧（服务端 C++17 / 客户端 C++11 / 测试共用）
//
// 仅 1505 请求与 1512 响应改为二进制体；其余消息（1501/1502/1506/1507/
// 1508/1509/1510 及头像 1601-1604）保持 JSON。三端必须同批发布（不兼容）。
//
// 帧体布局：[u16 meta_len 大端][meta JSON（meta_len 字节 UTF-8）][raw 字节]
//   - 1505 请求：meta = {message_id:"<dec>", offset:"<dec>", chunk_sha256:
//     "<64 lowercase hex>"}，raw = 分片原始字节（不再 Base64，可含内嵌 NUL）。
//   - 1512 响应：meta = {error:int, message_id:"<dec>", offset:"<dec>",
//     bytes:"<dec>", chunk_sha256:"<hex>", total_size:"<dec>", is_last:bool}，
//     raw = 分片原始字节；错误分支 raw 为空，meta 携带 error。
//   - 64 位字段线上仍为十进制字符串（与 JSON 协议同一约定）；
//     meta 必须是各端自行 parse 的 JSON 对象（本头不做 JSON 校验）。
//
// 预算（无符号边界，双向校验）：meta_len ∈ [1, 8KiB]，raw ≤ 256KiB，
// kMaxFrameBody = 2B 前缀 + 8KiB meta + 256KiB 分片。Encode 在全部校验
// 通过前不改 out；Decode 在全部校验通过前不改 meta/raw；各段先限界再求和，
// 避免 size_t 加法溢出。本头只依赖 C++11 标准库（cstdint/cstring/string）。
// =============================================================================
#include <cstdint>
#include <cstring>
#include <string>

namespace llfc_wire {

//分片大小（v2：256KiB；v1 为 32KiB JSON+Base64）
const std::uint32_t kResourceChunkSize = 256u * 1024u;

//meta_len 字段上限（meta JSON 预算；meta 必须非空——没有合法的空 JSON 对象）
const std::uint16_t kMaxMetaLen = 8u * 1024u;

//帧体上限 = 2B meta_len 前缀 + 8KiB meta 预算 + 256KiB 分片
const std::uint32_t kMaxFrameBody = 2u + 8u * 1024u + kResourceChunkSize;

//编码 [u16 meta_len BE][meta][raw]。拒绝：out 为空指针/空 meta（协议要求
//meta 为 JSON 对象，空串非法）/meta 超 kMaxMetaLen/raw 超 kResourceChunkSize/
//总帧超 kMaxFrameBody。全部校验通过后才写 out。
inline bool EncodeFrame(const std::string& meta, const std::string& raw,
    std::string* out)
{
    if (out == 0) {
        return false;
    }
    //各段先限界再求和，杜绝 size_t 溢出
    if (meta.empty() || meta.size() > kMaxMetaLen) {
        return false;
    }
    if (raw.size() > kResourceChunkSize) {
        return false;
    }
    const std::size_t total = static_cast<std::size_t>(2) + meta.size()
        + raw.size();
    if (total > static_cast<std::size_t>(kMaxFrameBody)) {
        return false;
    }
    const std::uint16_t meta_len = static_cast<std::uint16_t>(meta.size());
    const char head[2] = {
        static_cast<char>((meta_len >> 8) & 0xFF),
        static_cast<char>(meta_len & 0xFF)
    };
    out->clear();
    out->reserve(total);
    out->append(head, 2);
    out->append(meta);
    out->append(raw);
    return true;
}

//从缓冲前 2 字节读 meta_len（大端）；不足 2 字节返回 false。
inline bool PeekMetaLen(const char* buf, std::size_t len, std::uint16_t* meta_len)
{
    if (buf == 0 || meta_len == 0 || len < 2) {
        return false;
    }
    const unsigned char hi = static_cast<unsigned char>(buf[0]);
    const unsigned char lo = static_cast<unsigned char>(buf[1]);
    *meta_len = static_cast<std::uint16_t>((hi << 8) | lo);
    return true;
}

//整帧解码（要求缓冲恰好一帧且满足全部预算）。先总帧上下界，再 meta 预算
//（非 0 且 ≤kMaxMetaLen），余 raw ≤ kResourceChunkSize；长度减法在通过
//总帧上界后进行（无下溢）。全部校验通过前不改 meta/raw（二者可为 nullptr，
//按需只取一段）。
inline bool DecodeFrame(const std::string& frame, std::string* meta,
    std::string* raw)
{
    //总帧下界：2B 前缀 + 至少 1 字节 meta（meta 非空）；上界：帧体预算
    if (frame.size() < 3 || frame.size() > static_cast<std::size_t>(kMaxFrameBody)) {
        return false;
    }
    std::uint16_t meta_len = 0;
    if (!PeekMetaLen(frame.data(), frame.size(), &meta_len)) {
        return false;
    }
    if (meta_len == 0 || meta_len > kMaxMetaLen) {
        return false;
    }
    const std::size_t header = 2;
    //总帧上界已通过：header + meta_len ≤ frame.size() 不会有下溢风险，但仍
    //显式校验余量后再取 raw 长度
    if (frame.size() - header < static_cast<std::size_t>(meta_len)) {
        return false; //meta 不完整
    }
    const std::size_t raw_len = frame.size() - header - meta_len;
    if (raw_len > static_cast<std::size_t>(kResourceChunkSize)) {
        return false; //raw 超分片预算
    }
    if (meta != 0) {
        meta->assign(frame, header, meta_len);
    }
    if (raw != 0) {
        raw->assign(frame, header + meta_len, raw_len);
    }
    return true;
}

} // namespace llfc_wire
