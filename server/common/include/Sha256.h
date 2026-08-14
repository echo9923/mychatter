#pragma once
#include <string>

// 通用 SHA-256 摘要工具（OpenSSL libcrypto），供 ChatServer / ResourceServer
// 在资源消息链路上做逐分片与整文件完整性校验。
//
// 输出一律 64 字符小写 hex。与 PasswordHash 同在 llfc_server_common_crypto，
// 封装遵循同一约定：绝不抛异常（调用方运行在 LogicWorker/FileWorker 线程），
// 失败返回空串，调用方必须检查。
namespace llfc {

/// 计算内存缓冲区的 SHA-256，返回小写 hex；失败返回空串。
std::string Sha256Hex(const std::string& data);

/// 计算磁盘文件的 SHA-256（流式 1MiB 分块读取，避免大文件整读），
/// 文件不存在/不可读/读取失败返回空串。
std::string Sha256FileHex(const std::string& path);

/// content_hash 字段/协议字段的格式校验：恰好 64 个小写 hex 字符。
bool IsValidSha256Hex(const std::string& hex);

} // namespace llfc
