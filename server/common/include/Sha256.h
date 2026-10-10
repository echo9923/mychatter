#pragma once
#include <cstddef>
#include <string>

// OpenSSL EVP 摘要上下文的前置声明（全局作用域；EVP_MD_CTX 即其 typedef，
// 头文件不引入 <openssl/*.h>，保持依赖最小）
struct evp_md_ctx_st;

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

/// 只可移动的流式 SHA-256 上下文（封装 OpenSSL EVP）。
///
/// 供上传/下载会话做增量哈希：字节连续落盘时逐片 Update，完成时 FinalHex，
/// 避免整文件重读。语义约定与上方自由函数一致：绝不抛异常，失败返回
/// false/空串，调用方必须检查。
///
/// FinalHex 幂等：finalize 后再次调用返回同一结果，直到 Reset；
/// finalize 之后 Update 返回 false（需 Reset 后重新累计）。
class Sha256Stream {
public:
    Sha256Stream();
    ~Sha256Stream();
    Sha256Stream(Sha256Stream&& other) noexcept;
    Sha256Stream& operator=(Sha256Stream&& other) noexcept;
    Sha256Stream(const Sha256Stream&) = delete;
    Sha256Stream& operator=(const Sha256Stream&) = delete;

    /// 追加数据。上下文初始化失败/已 finalize/OOM 返回 false；
    /// 调用方应使增量状态失效并在完成点回退整文件重算。
    bool Update(const void* data, std::size_t len);
    bool Update(const std::string& data) { return Update(data.data(), data.size()); }

    /// 结束累计并返回小写 hex（64 字符）；失败/未初始化返回空串。
    std::string FinalHex();

    /// 复位为全新上下文，可重新 Update。
    void Reset();

private:
    void Destroy() noexcept;
    bool Init();

    evp_md_ctx_st* _ctx = nullptr;  ///< EVP_MD_CTX；构造即 DigestInit(sha256)
    std::string _final;             ///< FinalHex 结果缓存（幂等，Reset 清空）
};

/// content_hash 字段/协议字段的格式校验：恰好 64 个小写 hex 字符。
bool IsValidSha256Hex(const std::string& hex);

/// 密码学安全随机字节（OpenSSL RAND_bytes）。失败返回 false（调用方
/// 必须检查，不得退化为时间戳/伪随机）。P2 秒传挑战 nonce 使用。
bool RandomBytes(unsigned char* out, std::size_t len);

/// 常量时间字节比较（CRYPTO_memcmp）：相等返回 true。用于秒传证明等
/// 秘密比对，杜绝按字节提前退出造成的时序侧信道。
bool ConstantTimeEquals(const void* a, const void* b, std::size_t len);

} // namespace llfc
