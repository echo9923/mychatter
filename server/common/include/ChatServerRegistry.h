#pragma once

#include <string>

namespace llfc {

inline constexpr const char* kChatServerRegistryKey = "chatserver:registry";
inline constexpr const char* kChatServerLeasePrefix = "chatserver:lease:";

inline std::string ChatServerLeaseKey(const std::string& name) {
	return std::string(kChatServerLeasePrefix) + name;
}

} // namespace llfc
