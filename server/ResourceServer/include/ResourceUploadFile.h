#pragma once

#include <boost/filesystem/path.hpp>
#include <string>

namespace llfc {

// Recover a pending upload once when its worker session is reconstructed.
// The last chunk is unconfirmed after a restart, even if its length is complete.
bool RecoverUploadFile(const boost::filesystem::path& part_path,
    unsigned long long total_size, unsigned long long& received);

// Discard an unconfirmed suffix without extending the file or changing a prefix.
bool TruncateUploadFile(const boost::filesystem::path& part_path,
    unsigned long long received);

// Called only by the owning file worker. Advances received after write/readback.
int WriteUploadChunk(const boost::filesystem::path& part_path,
    unsigned long long total_size, unsigned long long offset,
    const std::string& data, const std::string& chunk_sha256,
    unsigned long long& received);

} // namespace llfc
