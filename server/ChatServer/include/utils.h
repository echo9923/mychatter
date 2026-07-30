#pragma once
#include <boost/date_time/posix_time/posix_time.hpp>
#include <sstream>
#include <string>

/**
 * @brief 获取当前时间戳字符串
 * @return 格式化的当前时间字符串（如 "2024-01-15 14:30:00"），用于聊天消息的时间记录
 */
std::string getCurrentTimestamp();