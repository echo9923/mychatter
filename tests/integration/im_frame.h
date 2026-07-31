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

// Parse the parsed body as JSON; tolerant (returns is_discarded() on failure).
inline json ParseJson(const std::string& s) {
	return json::parse(s, nullptr, false);
}

} // namespace imt
