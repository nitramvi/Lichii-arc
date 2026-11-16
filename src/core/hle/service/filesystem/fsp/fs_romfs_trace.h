// SPDX-FileCopyrightText: Copyright 2026 lichii-arc
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "core/file_sys/vfs/vfs_types.h"

namespace Service::FileSystem {

/// lichii-arc (LICHII_ZONE_PREWARM / LICHII_SHADER_PROVENANCE): which RomFS file a raw storage
/// read lands in. Built once per storage, on its first read, from the RomFS file table (a
/// storage that is not a RomFS image maps nothing). Reports the file to
/// Common::GameFileTrace whenever a read lands in another file than the previous one.
class RomfsReadTrace {
public:
    void OnRead(const FileSys::VirtualFile& storage, u64 offset);

private:
    struct Range {
        u64 begin;
        u64 end;
        u32 path;
    };

    void Build(const FileSys::VirtualFile& storage);

    std::mutex mutex;
    bool built{};
    std::vector<Range> ranges;
    std::vector<std::string> paths;
    u32 last_path{~0U};
};

} // namespace Service::FileSystem
