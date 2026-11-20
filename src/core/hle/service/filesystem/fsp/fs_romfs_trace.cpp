// SPDX-FileCopyrightText: Copyright 2026 lichii-arc
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>

#include "common/game_file_trace.h"
#include "common/logging.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/file_sys/vfs/vfs_offset.h"
#include "core/hle/service/filesystem/fsp/fs_romfs_trace.h"

namespace Service::FileSystem {

void RomfsReadTrace::Build(const FileSys::VirtualFile& storage) {
    built = true;
    if (!storage || storage->GetSize() < 0x50) {
        return;
    }
    FileSys::VirtualDir root;
    try {
        root = FileSys::ExtractRomFS(storage);
    } catch (...) {
        root = nullptr;
    }
    if (!root) {
        return;
    }
    const std::function<void(const FileSys::VirtualDir&, const std::string&)> walk =
        [&](const FileSys::VirtualDir& dir, const std::string& prefix) {
            for (const auto& file : dir->GetFiles()) {
                const auto* offset_file = dynamic_cast<const FileSys::OffsetVfsFile*>(file.get());
                if (offset_file == nullptr || file->GetSize() == 0) {
                    continue;
                }
                const u64 begin = offset_file->GetOffset();
                ranges.push_back({begin, begin + file->GetSize(), static_cast<u32>(paths.size())});
                paths.push_back(prefix + file->GetName());
            }
            for (const auto& sub : dir->GetSubdirectories()) {
                walk(sub, prefix + sub->GetName() + "/");
            }
        };
    walk(root, "");
    std::ranges::sort(ranges, {}, &Range::begin);
    // lichii-arc: LICHII_FILE_LOG=1 lists the folders (two levels) that hold the game's files, to
    // see which voice languages the dump carries.
    if (const char* value = std::getenv("LICHII_FILE_LOG"); value != nullptr && value[0] == '1') {
        std::map<std::string, size_t> folders;
        for (const auto& path : paths) {
            const size_t first = path.find('/');
            const size_t second =
                first == std::string::npos ? std::string::npos : path.find('/', first + 1);
            ++folders[second == std::string::npos ? path.substr(0, first) : path.substr(0, second)];
        }
        size_t logged = 0;
        for (const auto& [folder, count] : folders) {
            if (++logged > 800) {
                break;
            }
            LOG_INFO(Service_FS, "LICHII_FILE_LOG folder {} ({} files)", folder, count);
        }
    }
    LOG_INFO(Service_FS, "lichii-arc game file trace: {} RomFS files mapped in a storage of {} bytes",
             paths.size(), storage->GetSize());
}

void RomfsReadTrace::OnRead(const FileSys::VirtualFile& storage, u64 offset) {
    if (!Common::GameFileTrace::Enabled()) {
        return;
    }
    std::string path;
    {
        std::scoped_lock lock{mutex};
        if (!built) {
            Build(storage);
        }
        if (ranges.empty()) {
            return;
        }
        auto it = std::ranges::upper_bound(ranges, offset, {}, &Range::begin);
        if (it == ranges.begin()) {
            return;
        }
        --it;
        if (offset >= it->end || it->path == last_path) {
            return;
        }
        last_path = it->path;
        path = paths[it->path];
    }
    Common::GameFileTrace::Note(path);
}

} // namespace Service::FileSystem
