// im_frame.h — wire format helpers for the IM TCP/JSON protocol.
//
// Protocol (plan Verification.2): [2-byte big-endian id][2-byte big-endian len]
// [UTF-8 JSON body]. Header-only; depends only on nlohmann_json.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include <nlohmann/json.hpp>

namespace imt {

inline constexpr int HEAD_TOTAL_LEN = 4;  // 2 (id) + 2 (len)
inline constexpr int HEAD_ID_LEN    = 2;
inline constexpr int HEAD_DATA_LEN  = 2;

// ResourceServer wire format uses a 4-byte big-endian length (vs Chat's 2):
//   [2-byte id][4-byte int32 length][body]  → 6-byte header.
inline constexpr int RES_HEAD_TOTAL_LEN = 6;
inline constexpr int RES_HEAD_ID_LEN    = 2;
inline constexpr int RES_HEAD_DATA_LEN  = 4;

using json = nlohmann::json;

// Build a complete frame: [id][len][body]. Writes into `out`.
inline void EncodeFrame(short id, const std::string& body, std::string& out) {
	const std::size_t n = body.size();
	out.resize(HEAD_TOTAL_LEN + n);
	const unsigned short id_be = static_cast<unsigned short>(id);
	const unsigned short len_be = static_cast<unsigned short>(n);
	out[0] = static_cast<char>((id_be >> 8) & 0xFF);
	out[1] = static_cast<char>(id_be & 0xFF);
	out[2] = static_cast<char>((len_be >> 8) & 0xFF);
	out[3] = static_cast<char>(len_be & 0xFF);
	if (n) std::memcpy(&out[HEAD_TOTAL_LEN], body.data(), n);
}

struct Frame {
	short       id = 0;
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

// Build a Resource frame: [id][4-byte len][body]. Writes into `out`.
inline void EncodeResFrame(short id, const std::string& body, std::string& out) {
	const std::size_t n = body.size();
	out.resize(RES_HEAD_TOTAL_LEN + n);
	const unsigned short id_be = static_cast<unsigned short>(id);
	const std::uint32_t len_be = static_cast<std::uint32_t>(n);
	out[0] = static_cast<char>((id_be >> 8) & 0xFF);
	out[1] = static_cast<char>(id_be & 0xFF);
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

} // namespace imt
