#include "ResourceUploadFile.h"
#include "Sha256.h"
#include "const.h"

#include <boost/filesystem.hpp>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <cerrno>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

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

#ifndef _WIN32
/// POSIX：fsync 目录使其中已发生的条目变更（rename/link/unlink）持久化。
bool FsyncDirectory(const boost::filesystem::path& dir_path) {
    const std::string dir = dir_path.string();
    const int dfd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY);
    if (dfd < 0) {
        std::cerr << "ResourceServer: open dir for fsync failed dir="
            << dir << " errno=" << errno << std::endl;
        return false;
    }
    const bool ok = ::fsync(dfd) == 0;
    if (!ok) {
        std::cerr << "ResourceServer: dir fsync failed dir=" << dir
            << " errno=" << errno << std::endl;
    }
    ::close(dfd);
    return ok;
}
#endif

} // namespace

namespace llfc {

boost::filesystem::path ResourceUploadingPartPath(
    const boost::filesystem::path& root, long long message_id) {
    return root / "uploading" / (std::to_string(message_id) + ".part");
}

boost::filesystem::path ResourceBlobPath(const boost::filesystem::path& root,
    const std::string& sha256_hex) {
    return root / "blobs" / sha256_hex.substr(0, 2) / sha256_hex;
}

std::mutex& BlobInstallGcMutex() {
    static std::mutex m; //进程级单一互斥（取舍见头文件注释）
    return m;
}

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
    unsigned long long total_size, unsigned long long chunk_size,
    unsigned long long& received) {
    if (chunk_size == 0) return false;
    unsigned long long size = 0;
    if (!ReadLength(part_path, size)) return false;
    // Length alone cannot distinguish a complete write from an interrupted one.
    const unsigned long long resume = (size == 0 || size > total_size)
        ? 0 : ((size - 1) / chunk_size) * chunk_size;
    if (!TruncateUploadFile(part_path, resume)) return false;
    received = resume;
    return true;
}

int WriteUploadChunk(const boost::filesystem::path& part_path,
    unsigned long long total_size, unsigned long long chunk_size,
    unsigned long long offset, const std::string& data,
    const std::string& chunk_sha256, unsigned long long& received) {
    if (chunk_size == 0) return ErrorCodes::FileOffsetInvalid;
    const unsigned long long length = data.size();
    if (length == 0 || length > chunk_size) return ErrorCodes::FileSizeExceeded;
    if (offset % chunk_size != 0 || offset > total_size
        || length > total_size - offset) return ErrorCodes::FileOffsetInvalid;
    if (length != (std::min<unsigned long long>)(chunk_size, total_size - offset))
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

bool FlushFileToDisk(const boost::filesystem::path& path) {
#ifdef _WIN32
    const HANDLE file = CreateFileW(path.wstring().c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        std::cerr << "ResourceServer: FlushFileToDisk open failed path="
            << path.string() << " gle=" << GetLastError() << std::endl;
        return false;
    }
    const bool ok = FlushFileBuffers(file) != 0;
    if (!ok) {
        std::cerr << "ResourceServer: FlushFileBuffers failed path="
            << path.string() << " gle=" << GetLastError() << std::endl;
    }
    CloseHandle(file);
    return ok;
#else
    const int fd = ::open(path.string().c_str(), O_RDONLY);
    if (fd < 0) {
        std::cerr << "ResourceServer: FlushFileToDisk open failed path="
            << path.string() << " errno=" << errno << std::endl;
        return false;
    }
    const bool ok = ::fsync(fd) == 0;
    if (!ok) {
        std::cerr << "ResourceServer: fsync failed path=" << path.string()
            << " errno=" << errno << std::endl;
    }
    ::close(fd);
    return ok;
#endif
}

bool EnsureDirectoryDurable(const boost::filesystem::path& dir) {
	if (dir.empty()) return true;
#ifdef _WIN32
	boost::system::error_code ec;
	boost::filesystem::create_directories(dir, ec);
	if (ec && !boost::filesystem::is_directory(dir, ec)) {
		std::cerr << "ResourceServer: create_directories failed dir="
			<< dir.string() << " ec=" << ec.message() << std::endl;
		return false;
	}
	// Windows 无 POSIX 目录 fsync 等价物：目录条目的持久化由创建/移动 API 的
	// 写穿语义承担（publish 侧是 MoveFileExW WRITE_THROUGH）。
	return true;
#else
	// 1) 幂等创建：自底向上收集"本调用新建"的层级，逐级创建。
	boost::system::error_code ec;
	std::vector<boost::filesystem::path> missing;
	boost::filesystem::path cur = dir;
	while (!cur.empty() && !boost::filesystem::is_directory(cur, ec)) {
		missing.push_back(cur);
		const boost::filesystem::path parent = cur.parent_path();
		if (parent.empty() || parent == cur) break;
		cur = parent;
	}
	for (std::vector<boost::filesystem::path>::reverse_iterator it = missing.rbegin();
		it != missing.rend(); ++it) {
		boost::system::error_code mk_ec;
		boost::filesystem::create_directory(*it, mk_ec);
		if (mk_ec && !boost::filesystem::is_directory(*it, mk_ec)) {
			std::cerr << "ResourceServer: create_directory failed dir="
				<< it->string() << " ec=" << mk_ec.message() << std::endl;
			return false;
		}
	}
	// 2) 持久化整条祖先条目链：dir 自身的条目存在于其父目录里，父的条目又
	// 存在于祖父里——只 fsync 叶子目录不足以让"新建的链"在崩溃后仍然可达。
	// **必须遍历整条链而不是只 fsync 本次新建的层级**：
	//   - 上次 create_directory 成功但父目录 fsync 失败（本函数返回 false），
	//     重试时该目录已存在，"新建集合"为空 → 旧实现整条链一个都不 fsync；
	//   - ConfigMgr 启动时先 create_directories(root/blobs)，运行期首次上传
	//     再 Ensure(root/blobs/<xx>)：blobs 的条目在 root 里尚未落盘。
	// 自底向上直到文件系统根（相对路径则到"."，即解析基准目录）。
	for (boost::filesystem::path p = dir; ; p = p.parent_path()) {
		if (!FsyncDirectory(p)) return false;
		const boost::filesystem::path parent = p.parent_path();
		if (parent.empty() || parent == p) break;
	}
	return true;
#endif
}

bool DurableRename(const boost::filesystem::path& from,
    const boost::filesystem::path& to, bool replace, bool* already_exists) {
	if (already_exists != nullptr) *already_exists = false;
#ifdef _WIN32
    // MoveFileExW(WRITE_THROUGH)：按微软文档，返回时移动操作已写入磁盘
    //（对 copy+delete 形式的移动保证先落盘再返回），Windows 未提供 POSIX
    // 目录 fsync 的直接等价物。不带 REPLACE_EXISTING 时目标已存在即失败
    //（ERROR_ALREADY_EXISTS），满足 no-replace，无覆盖竞态。
    const DWORD flags = MOVEFILE_WRITE_THROUGH
        | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
    if (MoveFileExW(from.wstring().c_str(), to.wstring().c_str(), flags) == 0) {
        const DWORD gle = GetLastError();
        //P2 no-replace 去重命中：目标 blob 已存在即视为成功路径。
        //MoveFileExW 按目标存在/重名分别报 ERROR_ALREADY_EXISTS 与
        //ERROR_FILE_EXISTS，两者都表示"移动未发生但目标已在位"
        if (!replace && (gle == ERROR_ALREADY_EXISTS || gle == ERROR_FILE_EXISTS)
            && already_exists != nullptr) {
            *already_exists = true;
            return true;
        }
        std::cerr << "ResourceServer: MoveFileExW failed from=" << from.string()
            << " to=" << to.string() << " replace=" << replace
            << " gle=" << gle << std::endl;
        return false;
    }
    return true;
#else
    // 目标目录的条目本身必须已持久化：blobs/<xx>/ 由本次上传新建时，只
    // fsync 该目录不够（条目在 blobs/ 里），由 EnsureDirectoryDurable
    // 逐级 fsync 父目录。目录不存在时 link/rename 必然 ENOENT。
    if (!EnsureDirectoryDurable(to.parent_path())) return false;
    if (!replace) {
        // no-replace：link() 在目标已存在时原子失败（EEXIST）。link+unlink
        // 并非单步原子 rename：link 成功后目标立即对并发观察者可见。
        if (::link(from.string().c_str(), to.string().c_str()) != 0) {
            if (errno == EEXIST && already_exists != nullptr) {
                //P2 去重命中：blob 已存在（blob 存在即为可信内容）
                *already_exists = true;
                return true;
            }
            std::cerr << "ResourceServer: link failed from=" << from.string()
                << " to=" << to.string() << " errno=" << errno << std::endl;
            return false;
        }
        if (::unlink(from.string().c_str()) != 0) {
            // unlink 源失败：保留"目标 + 源"并返回 false——目标已是可信
            // 内容，绝不回收已公开目标；残留源由调用方/清理任务回收。
            std::cerr << "ResourceServer: unlink source failed (published target"
                " kept, stale source left for cleanup) from=" << from.string()
                << " errno=" << errno << std::endl;
            return false;
        }
    }
    else if (::rename(from.string().c_str(), to.string().c_str()) != 0) {
        std::cerr << "ResourceServer: rename failed from=" << from.string()
            << " to=" << to.string() << " errno=" << errno << std::endl;
        return false;
    }
    // 目录条目持久化：rename/unlink 改动源目录，link 改动目标目录。同目录
    // 移动 fsync 一次即可；跨目录移动（P2：uploading→blobs）需两个目录都 fsync。
    bool ok = FsyncDirectory(to.parent_path());
    if (from.parent_path() != to.parent_path()) {
        ok = FsyncDirectory(from.parent_path()) && ok;
    }
    return ok;
#endif
}

} // namespace llfc
