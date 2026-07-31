// SecurityUtil.h — Shared security primitives for mTLS ticket / session-token flow.
//
// Header-only: OpenSSL EVP (SHA-256), constant-time comparison, and a
// 128-bit session-token generator. Linked into Gate/Chat/Status/Resource via
// OpenSSL::Crypto. No secrets are ever logged by anything in this header.
#pragma once

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>

#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace security {

/// Compute SHA-256 of @p input and return the digest as 64 lowercase hex chars.
/// Returns an empty string on OpenSSL failure (caller treats as fail-closed).
inline std::string Sha256Hex(const std::string& input) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;

    const EVP_MD* md = EVP_sha256();
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    // OpenSSL >= 1.1.0: EVP_MD_CTX is opaque, so the heap _new/_free API is
    // mandatory. The legacy stack-based _init/_cleanup path is unreachable here.
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr) {
        return std::string();
    }
    bool ok = (EVP_DigestInit_ex(ctx, md, nullptr) == 1) &&
              (EVP_DigestUpdate(ctx, input.data(), input.size()) == 1) &&
              (EVP_DigestFinal_ex(ctx, digest, &digest_len) == 1);
    EVP_MD_CTX_free(ctx);
#else
    EVP_MD_CTX stack_ctx;
    EVP_MD_CTX* ctx = &stack_ctx;
    EVP_MD_CTX_init(ctx);
    bool ok = (EVP_DigestInit_ex(ctx, md, nullptr) == 1) &&
              (EVP_DigestUpdate(ctx, input.data(), input.size()) == 1) &&
              (EVP_DigestFinal_ex(ctx, digest, &digest_len) == 1);
    EVP_MD_CTX_cleanup(ctx);
#endif

    if (!ok || digest_len == 0) {
        return std::string();
    }

    static const char hex[] = "0123456789abcdef";
    std::string hex_out;
    hex_out.reserve(static_cast<size_t>(digest_len) * 2);
    for (unsigned int i = 0; i < digest_len; ++i) {
        hex_out.push_back(hex[digest[i] >> 4]);
        hex_out.push_back(hex[digest[i] & 0x0F]);
    }
    return hex_out;
}

/// Constant-time equality test. Returns false immediately when lengths differ
/// (length is not itself a secret). Otherwise XORs every byte through a
/// volatile accumulator so compilers cannot short-circuit.
inline bool ConstantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    volatile unsigned char acc = 0;
    const unsigned char* pa =
        reinterpret_cast<const unsigned char*>(a.data());
    const unsigned char* pb =
        reinterpret_cast<const unsigned char*>(b.data());
    for (size_t i = 0; i < a.size(); ++i) {
        acc |= static_cast<unsigned char>(pa[i] ^ pb[i]);
    }
    return acc == 0;
}

/// Generate a fresh 128-bit session token encoded as 32 lowercase hex chars.
/// Returns an empty string on RAND_bytes failure (caller treats as fail-closed).
inline std::string GenerateSessionToken() {
    unsigned char buf[16];
    if (RAND_bytes(buf, static_cast<int>(sizeof(buf))) != 1) {
        return std::string();
    }
    static const char hex[] = "0123456789abcdef";
    std::string token;
    token.reserve(sizeof(buf) * 2);
    for (size_t i = 0; i < sizeof(buf); ++i) {
        token.push_back(hex[buf[i] >> 4]);
        token.push_back(hex[buf[i] & 0x0F]);
    }
    return token;
}

}  // namespace security
