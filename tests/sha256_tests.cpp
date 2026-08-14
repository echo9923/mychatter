// SHA-256 helper tests (resource unified transfer).
//
// Plain main() consistent with the other suites: prints [PASS]/[FAIL] per
// assertion, exits non-zero on any failure. Compiles the production
// server/common/src/Sha256.cpp directly and links only libcrypto.
#include "Sha256.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_failures = 0;

#define CHECK(cond) do { \
    if (cond) { std::printf("[PASS] %s\n", #cond); } \
    else { ++g_failures; std::printf("[FAIL] %s (%s:%d)\n", #cond, __FILE__, __LINE__); } \
} while (0)

int main() {
    // NIST FIPS 180-2 test vectors (SHA-256)
    CHECK(llfc::Sha256Hex("") ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(llfc::Sha256Hex("abc") ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(llfc::Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    // IsValidSha256Hex: exactly 64 lowercase hex chars
    CHECK(llfc::IsValidSha256Hex(llfc::Sha256Hex("abc")));
    CHECK(!llfc::IsValidSha256Hex(""));
    CHECK(!llfc::IsValidSha256Hex("ABC"));                                // 长度
    CHECK(!llfc::IsValidSha256Hex(std::string(63, 'a')));                 // 63 位
    CHECK(!llfc::IsValidSha256Hex(std::string(65, 'a')));                 // 65 位
    CHECK(!llfc::IsValidSha256Hex(std::string(64, 'g')));                 // 非 hex 字符
    CHECK(!llfc::IsValidSha256Hex("BA7816BF8F01CFEA414140DE5DAE2223"      // 大写拒绝
        "B00361A396177A9CB410FF61F20015AD"));

    // 文件哈希：写入临时文件（含二进制 0 字节与多块内容）校验与内存版一致
    const std::string tmp_path = "sha256_tests_tmp.bin";
    std::string blob;
    blob.reserve(3000000);
    for (int i = 0; i < 3000000; ++i) {
        blob.push_back(static_cast<char>(i * 31 + (i >> 8)));
    }
    {
        FILE* fp = std::fopen(tmp_path.c_str(), "wb");
        CHECK(fp != nullptr);
        if (fp) {
            std::fwrite(blob.data(), 1, blob.size(), fp);
            std::fclose(fp);
        }
    }
    //3MB 覆盖 1MiB 流式分块边界；与内存版结果一致
    CHECK(llfc::Sha256FileHex(tmp_path) == llfc::Sha256Hex(blob));

    //不存在/不可读文件返回空串
    CHECK(llfc::Sha256FileHex("definitely_missing_file_zz.bin").empty());

    std::remove(tmp_path.c_str());

    if (g_failures == 0) {
        std::printf("[PASS] sha256_tests all checks\n");
        return 0;
    }
    std::printf("[FAIL] sha256_tests %d failure(s)\n", g_failures);
    return 1;
}
