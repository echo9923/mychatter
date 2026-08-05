#pragma once
#include <string>

// PBKDF2-HMAC-SHA256 password hashing (OpenSSL libcrypto), shared by
// GateServer / ChatServer / ResourceServer and the password_hash unit test.
//
// Storage format (self-describing):
//   "pbkdf2-sha256$i=<iter>$<salt_hex>$<dk_hex>"
// salt: 16 random bytes (RAND_bytes); dk: 32 bytes (256-bit derived key).
namespace llfc {

// Work factor. OWASP-recommended 600k iterations for PBKDF2-HMAC-SHA256.
inline constexpr int kPbkdf2Iterations = 600000;

// Hash `pw` with PBKDF2-HMAC-SHA256 and a fresh random salt.
// Returns the self-describing string above, or an EMPTY string if the
// OpenSSL backend fails (RAND_bytes / PBKDF2 error).
// Deliberately never throws: DAO call sites run on worker threads with no
// exception handler (LogicWorker), so callers must check for empty.
std::string HashPassword(const std::string& pw);

// Constant-time verification against `stored`, which must be a HashPassword()
// string (re-derives with the embedded salt/iterations). Returns false for
// empty or malformed `stored` (including legacy plaintext).
bool VerifyPassword(const std::string& pw, const std::string& stored);

} // namespace llfc
