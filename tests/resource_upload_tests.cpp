#include "ResourceUploadFile.h"
#include "Sha256.h"
#include "const.h"

#include <boost/filesystem.hpp>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

int failures = 0;

void Check(bool condition, const std::string& name) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", name.c_str());
    if (!condition) ++failures;
}

struct TestDirectory {
    boost::filesystem::path path = boost::filesystem::current_path()
        / boost::filesystem::unique_path("resource_upload_tests-%%%%-%%%%-%%%%");
    TestDirectory() { boost::filesystem::create_directory(path); }
    ~TestDirectory() {
        boost::system::error_code ec;
        boost::filesystem::remove_all(path, ec);
    }
};

std::string Blob(std::size_t size) {
    std::string data(size, '\0');
    for (std::size_t i = 0; i < size; ++i)
        data[i] = static_cast<char>((i * 31 + (i >> 9)) & 255);
    return data;
}

void Seed(const boost::filesystem::path& path, const std::string& data) {
    std::ofstream out(path.string(), std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();
    if (!out) throw std::runtime_error("cannot create test residue");
}

std::string Read(const boost::filesystem::path& path) {
    std::ifstream in(path.string(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

int SendChunk(const boost::filesystem::path& path, const std::string& blob,
    unsigned long long offset, unsigned long long& received) {
    const std::string chunk = blob.substr(static_cast<std::size_t>(offset), MAX_FILE_LEN);
    return llfc::WriteUploadChunk(path, blob.size(), offset, chunk,
        llfc::Sha256Hex(chunk), received);
}

void Finish(const boost::filesystem::path& path, const std::string& blob,
    unsigned long long& received, const std::string& name) {
    while (received < blob.size()) {
        const auto before = received;
        const int error = SendChunk(path, blob, before, received);
        Check(error == ErrorCodes::Success && received > before, name + ": chunk accepted");
        if (error != ErrorCodes::Success || received <= before) return;
    }
    Check(Read(path) == blob && llfc::Sha256FileHex(path.string()) == llfc::Sha256Hex(blob),
        name + ": resumed file is byte-identical and has the original SHA-256");
}

void TestRecovery(const boost::filesystem::path& root) {
    struct Case {
        const char* name;
        std::size_t total;
        std::size_t length;
        unsigned long long resume;
    };
    const Case cases[] = {
        {"empty", 100000, 0, 0},
        {"first-partial", 100000, 16384, 0},
        {"first-aligned", 100000, 32768, 0},
        {"two-aligned", 100000, 65536, 32768},
        {"third-partial", 100000, 81920, 65536},
        {"third-full-but-damaged", 100000, 98304, 65536},
        {"last-partial", 100000, 99999, 98304},
        {"full-pending", 100000, 100000, 98304},
        {"full-aligned-pending", 98304, 98304, 65536},
        {"one-byte-pending", 1, 1, 0},
        {"oversized-residue", 100000, 100001, 0}
    };
    for (const auto& test : cases) {
        const auto path = root / (std::string(test.name) + ".part");
        const auto blob = Blob(test.total);
        // Poison only the unconfirmed tail; earlier chunks must be preserved.
        std::string residue = blob.substr(0, static_cast<std::size_t>(test.resume));
        residue.resize(test.length, '?');
        Seed(path, residue);
        unsigned long long received = 999999;
        const bool recovered = llfc::RecoverUploadFile(path, blob.size(), received);
        Check(recovered && received == test.resume,
            std::string(test.name) + ": recovery returns the last chunk start");
        Check(boost::filesystem::file_size(path) == test.resume
            && Read(path) == blob.substr(0, static_cast<std::size_t>(test.resume)),
            std::string(test.name) + ": incomplete tail removed, prefix preserved");
        if (recovered) Finish(path, blob, received, test.name);
    }
    unsigned long long received = 123;
    const auto absent = root / "absent.part";
    Check(llfc::RecoverUploadFile(absent, 100000, received) && received == 0
        && !boost::filesystem::exists(absent), "missing upload starts at zero");
    const auto fresh = root / "new-sender" / "first-upload.part";
    received = 123;
    Check(llfc::RecoverUploadFile(fresh, 100000, received) && received == 0,
        "first upload recovers when its parent directory does not exist yet");
    if (received == 0) Finish(fresh, Blob(100000), received, "first upload");
}

void TestDuplicateRepair(const boost::filesystem::path& root) {
    const auto path = root / "duplicate.part";
    const auto blob = Blob(100000);
    Seed(path, blob.substr(0, 98304));
    unsigned long long received = 98304;
    Check(SendChunk(path, blob, 32768, received) == ErrorCodes::Success
        && received == 98304 && Read(path) == blob.substr(0, 98304),
        "valid duplicate preserves subsequent chunks and progress");

    const auto chunk = blob.substr(32768, 32768);
    const auto bad_data = std::string(chunk.size(), '?');
    Check(llfc::WriteUploadChunk(path, blob.size(), 32768, bad_data,
        llfc::Sha256Hex(chunk), received) == ErrorCodes::FileHashMismatch
        && received == 98304 && Read(path) == blob.substr(0, 98304),
        "bad incoming duplicate hash cannot discard a valid prefix");

    auto damaged = blob.substr(0, 98304);
    damaged[40000] ^= 0x55;
    Seed(path, damaged);
    Check(SendChunk(path, blob, 32768, received) == ErrorCodes::FileHashMismatch
        && received == 32768 && Read(path) == blob.substr(0, 32768),
        "damaged duplicate returns its start and discards only that chunk and suffix");
    Finish(path, blob, received, "duplicate repair");
}

void TestInvalidChunks(const boost::filesystem::path& root) {
    const auto path = root / "invalid.part";
    const auto blob = Blob(100000);
    Seed(path, blob.substr(0, 32768));
    unsigned long long received = 32768;
    Check(SendChunk(path, blob, 65536, received) == ErrorCodes::FileOffsetInvalid,
        "skip-ahead chunk rejected");
    Check(SendChunk(path, blob, 1, received) == ErrorCodes::FileOffsetInvalid,
        "non-boundary upload offset rejected");
    const auto short_chunk = blob.substr(32768, 16384);
    Check(llfc::WriteUploadChunk(path, blob.size(), 32768, short_chunk,
        llfc::Sha256Hex(short_chunk), received) == ErrorCodes::FileSizeExceeded,
        "short non-final request cannot create a partial confirmed chunk");
    Check(llfc::WriteUploadChunk(path, blob.size(),
        (std::numeric_limits<unsigned long long>::max)() - 32767,
        short_chunk, llfc::Sha256Hex(short_chunk), received) == ErrorCodes::FileOffsetInvalid,
        "overflowing offset rejected without writing");
    Check(received == 32768 && Read(path) == blob.substr(0, 32768),
        "invalid requests preserve progress and file bytes");
    Check(!llfc::TruncateUploadFile(path, 65536)
        && boost::filesystem::file_size(path) == 32768,
        "rollback never extends a file with holes");
}

void TestIoFailures(const boost::filesystem::path& root) {
    const auto blob = Blob(100000);
    const auto dir = root / "directory.part";
    boost::filesystem::create_directory(dir);
    unsigned long long received = 65536;
    Check(!llfc::RecoverUploadFile(dir, blob.size(), received) && received == 65536,
        "recovery I/O failure does not report a successful offset reset");
    Check(SendChunk(dir, blob, 65536, received) == ErrorCodes::FileWritePermissionFailed
        && received == 65536, "failed write keeps the retry at the original chunk start");

    const auto path = root / "failed-write.part";
    Seed(path, blob.substr(0, 65536) + std::string(16384, '?'));
#ifdef _WIN32
    const HANDLE locked = CreateFileW(path.wstring().c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, "lock upload file to force write/truncate errors");
    if (locked != INVALID_HANDLE_VALUE) {
        Check(!llfc::RecoverUploadFile(path, blob.size(), received)
            && received == 65536 && boost::filesystem::file_size(path) == 81920,
            "failed truncation retains the old confirmed progress");
        Check(SendChunk(path, blob, 65536, received) == ErrorCodes::FileWritePermissionFailed
            && received == 65536, "write failure with a half-written tail does not advance progress");
        CloseHandle(locked);
    }
#endif
    Check(llfc::TruncateUploadFile(path, received)
        && Read(path) == blob.substr(0, 65536), "discard failed write before retry, keep earlier chunks");
    Finish(path, blob, received, "write retry");
}

} // namespace

int main() {
    try {
        TestDirectory directory;
        TestRecovery(directory.path);
        TestDuplicateRepair(directory.path);
        TestInvalidChunks(directory.path);
        TestIoFailures(directory.path);
    } catch (const std::exception& e) {
        Check(false, e.what());
    }
    std::printf("resource_upload_tests: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
