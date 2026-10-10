#pragma once

#include <boost/filesystem/path.hpp>
#include <mutex>
#include <string>

namespace llfc {

/// P2 内容寻址存储路径（根路径来自 ConfigMgr `[Storage]/[Output] ResourceRoot`，
/// 缺省 `<Output.Path>/resource`）：
///   uploading/<message_id>.part   进行中上传（不再按发送者分目录）
///   blobs/<sha[0:2]>/<sha256>     已发布内容寻址 blob
/// blob 只会在 哈希校验通过 → flush → no-replace 持久化重命名 之后出现，
/// 但"同名文件存在"不等于"内容可信"：去重命中必须复核长度+整文件哈希+耐久性。
/// 三端（FileWorker/ResourceLogicWorker/清理任务）共用同一推导，避免分叉。
boost::filesystem::path ResourceUploadingPartPath(
    const boost::filesystem::path& root, long long message_id);
boost::filesystem::path ResourceBlobPath(const boost::filesystem::path& root,
    const std::string& sha256_hex);

/// P2 CAS/GC 互斥：blob 安装（no-replace rename → DB 发布事务提交）与清理
/// 任务的"孤儿判定查询 + unlink"必须互斥，否则 GC 可能在发布提交前删掉
/// 刚安装的 blob。取舍记录：采用进程级单一互斥（非 per-sha）——实现最简、
/// 语义最易审计；代价是"发布完成点"与"GC 单次孤儿判定"互相串行，吞吐影响
/// 未经测量（无 bench 数据，不假设可忽略）。因此本锁只覆盖必需区间：清理
/// 任务持锁期间只做一次 typed EXISTS 查询与一次 unlink，发布侧只在
/// rename→DB commit 之间持锁。该锁不跨进程，同一 CAS 根目录只能由一个
/// ResourceServer 进程独占。
std::mutex& BlobInstallGcMutex();

// Recover a pending upload once when its worker session is reconstructed.
// The last chunk is unconfirmed after a restart, even if its length is
// complete. chunk_size is the wire chunk size the sender uses (P1a: 32KiB
// JSON+Base64; P1c will raise it) — recovery rolls back to the last chunk
// boundary measured in that size.
bool RecoverUploadFile(const boost::filesystem::path& part_path,
    unsigned long long total_size, unsigned long long chunk_size,
    unsigned long long& received);

// Discard an unconfirmed suffix without extending the file or changing a prefix.
bool TruncateUploadFile(const boost::filesystem::path& part_path,
    unsigned long long received);

// Called only by the owning file worker. Advances received after write/readback.
// chunk_size must equal the sender's chunk size (see RecoverUploadFile).
int WriteUploadChunk(const boost::filesystem::path& part_path,
    unsigned long long total_size, unsigned long long chunk_size,
    unsigned long long offset, const std::string& data,
    const std::string& chunk_sha256, unsigned long long& received);

// Flush a file's data (and metadata) to physical storage: Windows
// FlushFileBuffers, POSIX fsync. Use before publishing/renaming a finished
// .part so a crash cannot publish truncated content.
bool FlushFileToDisk(const boost::filesystem::path& path);

// Ensure dir exists and that the *directory entry chain it needs* is durable.
// POSIX: a directory's own entry lives in its parent, which lives in its
// parent's parent…, so fsyncing only the leaf (or only the levels created by
// this very call) is not enough: the whole chain from dir up to the filesystem
// root is fsynced on every call — covering the retry case (a previous call
// created the directory but failed to fsync its parent, so nothing looks
// "newly created" now) and the startup case (ConfigMgr created root/blobs,
// and the entry of blobs inside root is not durable yet). Cost is a handful of
// directory fsyncs per call (path depth); it is on the blob-install path and
// therefore serialized with GC — the actual throughput impact is unmeasured.
// Relative paths fsync "." as their anchor. Windows: directories have no
// direct fsync equivalent (MoveFileExW WRITE_THROUGH covers the file), so this
// only creates the tree and reports false when the tree cannot be created.
bool EnsureDirectoryDurable(const boost::filesystem::path& dir);

// Durable rename: Windows MoveFileExW(MOVEFILE_WRITE_THROUGH); POSIX rename(2)
// (replace) or link+unlink (no-replace), then fsync of the target's directory
// plus the source's when the move crosses directories. replace=false never
// overwrites an existing target (POSIX link fails atomically with EEXIST),
// but link+unlink is not a single atomic rename: once link succeeds the
// target is immediately visible to concurrent observers. If the source
// unlink then fails, the published target is kept (never reclaimed) and the
// stale source is left for cleanup. A false return therefore does not mean
// nothing changed: the target may already be in place with either durability
// unconfirmed (fsync failed) or the source unlink failed.
/// already_exists（可选出参）：no-replace 且目标已存在时置真并返回 true
/// （P2 去重命中入口：目标存在 ≠ 内容可信，调用方必须自行复核长度 +
/// 整文件哈希 + 耐久性后才能复用）。其余失败仍返回 false。
bool DurableRename(const boost::filesystem::path& from,
    const boost::filesystem::path& to, bool replace,
    bool* already_exists = nullptr);

} // namespace llfc
