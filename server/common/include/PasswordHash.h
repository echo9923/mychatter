#pragma once
#include <string>

// PBKDF2-HMAC-SHA256 password hashing (OpenSSL libcrypto), shared by
// GateServer / ChatServer / ResourceServer and the password_hash unit test.
//
// Storage format (self-describing, so the work factor can be raised later
// without invalidating existing hashes):
//   "pbkdf2-sha256$i=<iter>$<salt_hex>$<dk_hex>"
// salt: 16 random bytes (RAND_bytes); dk: 32 bytes (256-bit derived key).
namespace llfc {

// Work factor. OWASP-recommended 600k iterations for PBKDF2-HMAC-SHA256.
// Bump this constant to upgrade older hashes: ShouldRehash() detects the
// mismatch and re-derives the credential on the next successful login.
inline constexpr int kPbkdf2Iterations = 600000;

// Hash `pw` with PBKDF2-HMAC-SHA256 and a fresh random salt.
// Returns the self-describing string above, or an EMPTY string if the
// OpenSSL backend fails (RAND_bytes / PBKDF2 error).
// Deliberately never throws: DAO call sites run on worker threads with no
// exception handler (LogicWorker), so callers must check for empty.
std::string HashPassword(const std::string& pw);

// Constant-time verification against `stored`. Accepts either a
// HashPassword() string (re-derives with the embedded salt/iterations) or a
// legacy plaintext value (direct constant-time compare, so pre-existing rows
// keep working and get migrated by rehash-on-login). Returns false for empty
// or malformed `stored`.
bool VerifyPassword(const std::string& pw, const std::string& stored);

// True when the row should be re-derived: legacy plaintext, or the stored
// iteration count differs from kPbkdf2Iterations.
bool ShouldRehash(const std::string& stored);

} // namespace llfc
