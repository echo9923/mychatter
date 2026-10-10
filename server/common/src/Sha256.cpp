#include "Sha256.h"

#include <openssl/evp.h>
#include <openssl/rand.h>  //RAND_bytes（P2 秒传 nonce）
#include <openssl/crypto.h>  //CRYPTO_memcmp（P2 常量时间比较）

#include <array>
#include <cstddef>
#include <cstdio>

namespace llfc {
namespace {

constexpr std::size_t kFileChunkBytes = 1024 * 1024; // 1MiB 流式读取粒度

std::string ToLowerHex(const unsigned char* data, std::size_t len) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        out.push_back(kHex[data[i] >> 4]);
        out.push_back(kHex[data[i] & 0x0F]);
    }
    return out;
}

} // namespace

std::string Sha256Hex(const std::string& data) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> md{};
    unsigned int md_len = 0;
    if (EVP_Digest(data.data(), data.size(), md.data(), &md_len, EVP_sha256(), nullptr) != 1) {
        return std::string();
    }
    return ToLowerHex(md.data(), md_len);
}

std::string Sha256FileHex(const std::string& path) {
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (fp == nullptr) {
        return std::string();
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr) {
        std::fclose(fp);
        return std::string();
    }

    std::string result;
    std::string buf(kFileChunkBytes, '\0');
    bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1;
    while (ok) {
        const std::size_t n = std::fread(&buf[0], 1, buf.size(), fp);
        if (n == 0) {
            ok = std::feof(fp) != 0; // 正常读完；读错误则失败
            break;
        }
        ok = EVP_DigestUpdate(ctx, buf.data(), n) == 1;
    }

    if (ok) {
        std::array<unsigned char, EVP_MAX_MD_SIZE> md{};
        unsigned int md_len = 0;
        if (EVP_DigestFinal_ex(ctx, md.data(), &md_len) == 1) {
            result = ToLowerHex(md.data(), md_len);
        }
    }

    EVP_MD_CTX_free(ctx);
    std::fclose(fp);
    return result;
}

bool IsValidSha256Hex(const std::string& hex) {
    if (hex.size() != 64) {
        return false;
    }
    for (const char c : hex) {
        const bool is_lower_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!is_lower_hex) {
            return false;
        }
    }
    return true;
}

bool RandomBytes(unsigned char* out, std::size_t len) {
    if (out == nullptr || len == 0) {
        return false;
    }
    //RAND_bytes 密码学安全；失败（熵源不可用等）如实返回，调用方不得降级
    return RAND_bytes(out, static_cast<int>(len)) == 1;
}

bool ConstantTimeEquals(const void* a, const void* b, std::size_t len) {
    if (len == 0) {
        return true;
    }
    if (a == nullptr || b == nullptr) {
        return false;
    }
    return CRYPTO_memcmp(a, b, len) == 0;
}

// --- Sha256Stream：只可移动的流式上下文 ----------------------------------------

Sha256Stream::Sha256Stream() {
    Init();
}

Sha256Stream::~Sha256Stream() {
    Destroy();
}

Sha256Stream::Sha256Stream(Sha256Stream&& other) noexcept
    : _ctx(other._ctx), _final(std::move(other._final)) {
    other._ctx = nullptr;
    other._final.clear();
}

Sha256Stream& Sha256Stream::operator=(Sha256Stream&& other) noexcept {
    if (this != &other) {
        Destroy();
        _ctx = other._ctx;
        _final = std::move(other._final);
        other._ctx = nullptr;
        other._final.clear();
    }
    return *this;
}

bool Sha256Stream::Init() {
    if (_ctx != nullptr) {
        return true; // 已初始化（Reset 复用）
    }
    _ctx = EVP_MD_CTX_new();
    if (_ctx == nullptr) {
        return false;
    }
    if (EVP_DigestInit_ex(_ctx, EVP_sha256(), nullptr) != 1) {
        Destroy();
        return false;
    }
    return true;
}

void Sha256Stream::Destroy() noexcept {
    if (_ctx != nullptr) {
        EVP_MD_CTX_free(_ctx);
        _ctx = nullptr;
    }
    _final.clear();
}

bool Sha256Stream::Update(const void* data, std::size_t len) {
    if (_ctx == nullptr || !_final.empty()) {
        // 未初始化（构造失败后）或已 finalize：不接受追加
        return false;
    }
    return EVP_DigestUpdate(_ctx, data, len) == 1;
}

std::string Sha256Stream::FinalHex() {
    if (!_final.empty()) {
        return _final; // 幂等：Reset 前重复 finalize 返回同一结果
    }
    if (_ctx == nullptr) {
        return std::string();
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> md{};
    unsigned int md_len = 0;
    if (EVP_DigestFinal_ex(_ctx, md.data(), &md_len) != 1) {
        return std::string();
    }
    _final = ToLowerHex(md.data(), md_len);
    return _final;
}

void Sha256Stream::Reset() {
    Destroy();
    Init();
}

} // namespace llfc
