#pragma once

#include <boost/filesystem/path.hpp>
#include <string>

namespace llfc {

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
bool DurableRename(const boost::filesystem::path& from,
    const boost::filesystem::path& to, bool replace);

} // namespace llfc
