// im_frame.h — wire format helpers for the IM TCP/JSON protocol.
//
// Protocol (plan Verification.2): [2-byte big-endian type][2-byte big-endian len]
// [UTF-8 JSON body]. Header-only; depends only on nlohmann_json.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include <nlohmann/json.hpp>

//共享线协议头必须在任何命名空间之前全局引入（llfc_wire 为全局命名空间）
#include "resource_wire.h"

namespace imt {

inline constexpr int HEAD_TOTAL_LEN = 4;  // 2 (type) + 2 (len)
inline constexpr int HEAD_TYPE_LEN  = 2;
inline constexpr int HEAD_DATA_LEN  = 2;

// ResourceServer wire format uses a 4-byte big-endian length (vs Chat's 2):
//   [2-byte type][4-byte int32 length][body]  → 6-byte header.
inline constexpr int RES_HEAD_TOTAL_LEN = 6;
inline constexpr int RES_HEAD_TYPE_LEN  = 2;
inline constexpr int RES_HEAD_DATA_LEN  = 4;

using json = nlohmann::json;

// Build a complete frame: [type][len][body]. Writes into `out`.
inline void EncodeFrame(short type, const std::string& body, std::string& out) {
	const std::size_t n = body.size();
	out.resize(HEAD_TOTAL_LEN + n);
	const unsigned short type_be = static_cast<unsigned short>(type);
	const unsigned short len_be = static_cast<unsigned short>(n);
	out[0] = static_cast<char>((type_be >> 8) & 0xFF);
	out[1] = static_cast<char>(type_be & 0xFF);
	out[2] = static_cast<char>((len_be >> 8) & 0xFF);
	out[3] = static_cast<char>(len_be & 0xFF);
	if (n) std::memcpy(&out[HEAD_TOTAL_LEN], body.data(), n);
}

struct Frame {
	short       type = 0;
	std::string body;
};

// Read a 2-byte big-endian short from a buffer.
inline short ReadBE16(const char* p) {
	return static_cast<short>(
		(static_cast<unsigned short>(static_cast<unsigned char>(p[0])) << 8) |
		 static_cast<unsigned char>(p[1]));
}

// Read a 4-byte big-endian unsigned int32 from a buffer.
inline std::uint32_t ReadBE32(const char* p) {
	return (static_cast<std::uint32_t>(static_cast<unsigned char>(p[0])) << 24) |
	       (static_cast<std::uint32_t>(static_cast<unsigned char>(p[1])) << 16) |
	       (static_cast<std::uint32_t>(static_cast<unsigned char>(p[2])) <<  8) |
	        static_cast<unsigned char>(p[3]);
}

// Build a Resource frame: [type][4-byte len][body]. Writes into `out`.
inline void EncodeResFrame(short type, const std::string& body, std::string& out) {
	const std::size_t n = body.size();
	out.resize(RES_HEAD_TOTAL_LEN + n);
	const unsigned short type_be = static_cast<unsigned short>(type);
	const std::uint32_t len_be = static_cast<std::uint32_t>(n);
	out[0] = static_cast<char>((type_be >> 8) & 0xFF);
	out[1] = static_cast<char>(type_be & 0xFF);
	out[2] = static_cast<char>((len_be >> 24) & 0xFF);
	out[3] = static_cast<char>((len_be >> 16) & 0xFF);
	out[4] = static_cast<char>((len_be >>  8) & 0xFF);
	out[5] = static_cast<char>(len_be & 0xFF);
	if (n) std::memcpy(&out[RES_HEAD_TOTAL_LEN], body.data(), n);
}

// Parse the parsed body as JSON; tolerant (returns is_discarded() on failure).
inline json ParseJson(const std::string& s) {
	return json::parse(s, nullptr, false);
}

// ---- P1c resource wire v2: [u16 meta_len BE][meta JSON][raw] --------------
// 常量与编解码在 proto/resource_wire.h（见文件顶部全局 include）。

// Build a 1505 binary chunk-upload body: meta = {message_id, offset,
// chunk_sha256}, raw = chunk bytes (no Base64). Returns false if meta overflows.
inline bool BuildChunkUploadReqBody(long long message_id, long long offset,
    const std::string& chunk_sha256, const std::string& raw, std::string* out) {
	json meta;
	meta["message_id"] = std::to_string(message_id);
	meta["offset"] = std::to_string(offset);
	meta["chunk_sha256"] = chunk_sha256;
	return llfc_wire::EncodeFrame(meta.dump(), raw, out);
}

// Parse a 1512 binary chunk-down body into meta JSON + raw chunk bytes.
inline bool ParseChunkDownRspBody(const std::string& body, json* meta,
    std::string* raw) {
	std::string meta_str;
	if (!llfc_wire::DecodeFrame(body, &meta_str, raw)) {
		return false;
	}
	*meta = json::parse(meta_str, nullptr, false);
	return !meta->is_discarded() && meta->is_object();
}


} // namespace imt
