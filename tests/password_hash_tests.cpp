// password_hash_tests — PBKDF2-HMAC-SHA256 helper unit tests.
//
// No test framework: plain main() prints [PASS]/[FAIL] and exits non-zero on
// any failure (mirrors worker_pool_tests). Compiles the production
// PasswordHash.cpp directly and links only OpenSSL::Crypto. The two
// HashPassword() calls (600k iterations each) cost roughly a second total.
#include "PasswordHash.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace llfc;

namespace {
int g_failures = 0;

void Check(bool cond, const char* name) {
    if (cond) { std::printf("[PASS] %s\n", name); }
    else { std::printf("[FAIL] %s\n", name); ++g_failures; }
}
} // namespace

int main() {
    const std::string pw   = "correct horse battery staple";
    const std::string wrong = "wrong";

    const std::string h1 = HashPassword(pw);
    Check(!h1.empty(), "HashPassword produced a non-empty value");
    Check(h1.rfind("pbkdf2-sha256$i=", 0) == 0, "self-describing prefix");
    Check(VerifyPassword(pw, h1), "round-trip HashPassword->VerifyPassword");
    Check(!VerifyPassword(wrong, h1), "wrong password rejected");

    const std::string h2 = HashPassword(pw);
    Check(h1 != h2, "same pw, two hashes differ (fresh salt)");
    Check(VerifyPassword(pw, h2), "second hash still verifies");

    Check(VerifyPassword("legacy", "legacy"), "legacy plaintext verifies");
    Check(!VerifyPassword("other", "legacy"), "legacy plaintext mismatch");
    Check(ShouldRehash("legacy"), "legacy plaintext flagged for rehash");

    Check(!VerifyPassword(pw, ""), "empty stored rejected");
    Check(!VerifyPassword(pw, "pbkdf2-sha256$i=600000$zz$00"), "bad salt hex rejected");
    Check(!VerifyPassword(pw, "pbkdf2-sha256$i=600000$00$zz"), "bad dk hex rejected");
    Check(!VerifyPassword(pw, "pbkdf2-sha256$i=-5$00$00"), "negative iter rejected");
    Check(!VerifyPassword(pw, "pbkdf2-sha256$i=0$00$00"), "zero iter rejected");
    Check(!VerifyPassword(pw, "pbkdf2-sha256"), "truncated stored rejected");

    // Tamper only the iteration count of a valid hash: same salt/dk, wrong i.
    std::string older = h1;
    const std::string marker = "i=";
    const std::size_t pos = older.find(marker);
    const std::size_t end = older.find('$', pos);
    older.replace(pos, end - pos, "i=10000");
    Check(ShouldRehash(older), "mismatched iteration flagged for rehash");
    Check(!VerifyPassword(pw, older), "mismatched iteration fails verify");

    const std::string weird("p\x01\x02\xfe\xff\x00ss");
    Check(VerifyPassword(weird, HashPassword(weird)), "binary pw round-trip");

    if (g_failures == 0) { std::printf("all password_hash tests passed\n"); }
    return g_failures ? 1 : 0;
}
