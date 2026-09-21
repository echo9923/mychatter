#include "ResourceUploadFile.h"
#include "Sha256.h"
#include "const.h"

#include <boost/filesystem.hpp>
#include <algorithm>
#include <fstream>
#include <iostream>

namespace {

constexpr unsigned long long kChunkSize = MAX_FILE_LEN;

bool ReadLength(const boost::filesystem::path& path, unsigned long long& size) {
    boost::system::error_code ec;
    const auto status = boost::filesystem::status(path, ec);
    if (status.type() == boost::filesystem::file_not_found) {
        size = 0;
        return true;
    }
    if (ec) return false;
    const auto length = boost::filesystem::file_size(path, ec);
    if (ec) return false;
    size = static_cast<unsigned long long>(length);
    return true;
}

std::string HashRange(const boost::filesystem::path& path,
    unsigned long long offset, std::size_t length) {
    std::ifstream in(path.string(), std::ios::binary);
    if (!in) return {};
    in.seekg(static_cast<std::streamoff>(offset));
    std::string data(length, '\0');
    in.read(&data[0], static_cast<std::streamsize>(length));
    if (in.gcount() != static_cast<std::streamsize>(length)) return {};
    return llfc::Sha256Hex(data);
}

bool WriteAndVerify(const boost::filesystem::path& path,
    unsigned long long offset, const std::string& data) {
    std::fstream io(path.string(), std::ios::binary | std::ios::in | std::ios::out);
    if (!io) {
        if (offset != 0) return false;
        std::ofstream create(path.string(), std::ios::binary | std::ios::app);
        if (!create) return false;
        create.close();
        io.clear();
        io.open(path.string(), std::ios::binary | std::ios::in | std::ios::out);
        if (!io) return false;
    }
    io.seekp(static_cast<std::streamoff>(offset));
    io.write(data.data(), static_cast<std::streamsize>(data.size()));
    io.flush();
    if (!io) return false;
    io.seekg(static_cast<std::streamoff>(offset));
    std::string readback(data.size(), '\0');
    io.read(&readback[0], static_cast<std::streamsize>(readback.size()));
    return io.gcount() == static_cast<std::streamsize>(data.size()) && readback == data;
}

} // namespace

namespace llfc {

bool TruncateUploadFile(const boost::filesystem::path& part_path,
    unsigned long long received) {
    unsigned long long size = 0;
    if (!ReadLength(part_path, size) || size < received) return false;
    if (size == received) return true;
    boost::system::error_code ec;
    boost::filesystem::resize_file(part_path, received, ec);
    if (ec) {
        std::cerr << "ResourceServer: truncate failed path=" << part_path.string()
            << " offset=" << received << " ec=" << ec.message() << std::endl;
        return false;
    }
    return true;
}

bool RecoverUploadFile(const boost::filesystem::path& part_path,
    unsigned long long total_size, unsigned long long& received) {
    unsigned long long size = 0;
    if (!ReadLength(part_path, size)) return false;
    // Length alone cannot distinguish a complete write from an interrupted one.
    const unsigned long long resume = (size == 0 || size > total_size)
        ? 0 : ((size - 1) / kChunkSize) * kChunkSize;
    if (!TruncateUploadFile(part_path, resume)) return false;
    received = resume;
    return true;
}

int WriteUploadChunk(const boost::filesystem::path& part_path,
    unsigned long long total_size, unsigned long long offset,
    const std::string& data, const std::string& chunk_sha256,
    unsigned long long& received) {
    const unsigned long long length = data.size();
    if (length == 0 || length > kChunkSize) return ErrorCodes::FileSizeExceeded;
    if (offset % kChunkSize != 0 || offset > total_size
        || length > total_size - offset) return ErrorCodes::FileOffsetInvalid;
    if (length != (std::min<unsigned long long>)(kChunkSize, total_size - offset))
        return ErrorCodes::FileSizeExceeded;
    if (offset > received || (offset < received && offset + length > received))
        return ErrorCodes::FileOffsetInvalid;
    if (!IsValidSha256Hex(chunk_sha256) || Sha256Hex(data) != chunk_sha256)
        return ErrorCodes::FileHashMismatch;

    if (offset < received) {
        const auto disk_hash = HashRange(part_path, offset, data.size());
        if (disk_hash.empty()) return ErrorCodes::FileReadFailed;
        if (disk_hash == chunk_sha256) return ErrorCodes::Success;
        // Return the chunk start so the client's error path rewinds its window.
        if (!TruncateUploadFile(part_path, offset)) return ErrorCodes::FileWritePermissionFailed;
        received = offset;
        return ErrorCodes::FileHashMismatch;
    }

    boost::system::error_code ec;
    const auto dir = part_path.parent_path();
    if (!dir.empty()) {
        boost::filesystem::create_directories(dir, ec);
        if (ec) return ErrorCodes::CreateFilePathFailed;
    }
    if (!WriteAndVerify(part_path, offset, data)) {
        // Close the stream before removing a partial write (required on Windows).
        TruncateUploadFile(part_path, received);
        return ErrorCodes::FileWritePermissionFailed;
    }
    received += length;
    return ErrorCodes::Success;
}

} // namespace llfc
