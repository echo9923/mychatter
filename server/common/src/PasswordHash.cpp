#include "PasswordHash.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cstddef>
#include <string>
#include <vector>

namespace llfc {
namespace {

constexpr const char* kHashPrefix = "pbkdf2-sha256";
constexpr int kSaltBytes = 16;
constexpr int kDerivedBytes = 32;

// ---- hex helpers ------------------------------------------------------------
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

int HexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool FromHex(const std::string& in, std::vector<unsigned char>& out) {
    if (in.empty() || in.size() % 2 != 0) return false;
    out.resize(in.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const int hi = HexNibble(in[i * 2]);
        const int lo = HexNibble(in[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<unsigned char>((hi << 4) | lo);
    }
    return true;
}

// ---- parsing ----------------------------------------------------------------
// Stored format: pbkdf2-sha256$i=<iter>$<salt_hex>$<dk_hex>
struct ParsedHash {
    int iterations = 0;
    std::vector<unsigned char> salt;
    std::vector<unsigned char> dk;
};

bool ParseHash(const std::string& stored, ParsedHash& out) {
    std::vector<std::string> parts;
    std::string cur;
    for (const char c : stored) {
        if (c == '$') { parts.push_back(cur); cur.clear(); }
        else { cur.push_back(c); }
    }
    parts.push_back(cur);
    if (parts.size() != 4) return false;
    if (parts[0] != kHashPrefix) return false;
    if (parts[1].size() < 3 || parts[1].compare(0, 2, "i=") != 0) return false;

    long iter = 0;
    try {
        std::size_t consumed = 0;
        iter = std::stol(parts[1].substr(2), &consumed, 10);
        if (consumed != parts[1].size() - 2) return false;
    } catch (...) { return false; }
    if (iter <= 0) return false;  // reject 0 / negative / sign tricks
    out.iterations = static_cast<int>(iter);

    if (!FromHex(parts[2], out.salt)) return false;
    if (!FromHex(parts[3], out.dk)) return false;
    return true;
}

// ---- primitives -------------------------------------------------------------
bool ConstantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;  // length is not secret
    // CRYPTO_memcmp returns 0 when the two buffers are identical.
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

bool DeriveKey(const std::string& pw, const std::vector<unsigned char>& salt,
               int iterations, std::vector<unsigned char>& dk) {
    dk.assign(kDerivedBytes, 0);
    const int rc = PKCS5_PBKDF2_HMAC(
        pw.data(), static_cast<int>(pw.size()),
        salt.data(), static_cast<int>(salt.size()),
        iterations, EVP_sha256(),
        kDerivedBytes, dk.data());
    return rc == 1;
}

} // namespace

std::string HashPassword(const std::string& pw) {
    unsigned char salt[kSaltBytes];
    if (RAND_bytes(salt, kSaltBytes) != 1) return std::string();

    const std::vector<unsigned char> salt_vec(salt, salt + kSaltBytes);
    std::vector<unsigned char> dk;
    if (!DeriveKey(pw, salt_vec, kPbkdf2Iterations, dk)) return std::string();

    std::string out;
    out.reserve(128);
    out += kHashPrefix;
    out += "$i=";
    out += std::to_string(kPbkdf2Iterations);
    out += "$";
    out += ToLowerHex(salt, kSaltBytes);
    out += "$";
    out += ToLowerHex(dk.data(), dk.size());
    return out;
}

bool VerifyPassword(const std::string& pw, const std::string& stored) {
    if (stored.empty()) return false;
    ParsedHash parsed;
    if (!ParseHash(stored, parsed)) {
        // Legacy plaintext row: constant-time compare against the raw value.
        return ConstantTimeEquals(pw, stored);
    }
    std::vector<unsigned char> dk;
    if (!DeriveKey(pw, parsed.salt, parsed.iterations, dk)) return false;
    if (dk.size() != parsed.dk.size()) return false;
    return CRYPTO_memcmp(dk.data(), parsed.dk.data(), dk.size()) == 0;
}

bool ShouldRehash(const std::string& stored) {
    ParsedHash parsed;
    if (!ParseHash(stored, parsed)) return true;  // legacy plaintext
    return parsed.iterations != kPbkdf2Iterations;
}

} // namespace llfc
