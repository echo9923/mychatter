#include "Sha256.h"

#include <openssl/evp.h>

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

} // namespace llfc
