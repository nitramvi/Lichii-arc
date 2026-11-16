// SPDX-FileCopyrightText: Copyright 2026 lichii-arc
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"

namespace Common::FS {
class IOFile;
}

namespace VideoCommon {

/// lichii-arc: disk cache of ASTC textures already transcoded to BC1/BC3 on the GPU.
///
/// A GPU without ASTC (Intel Arc 140V) decodes every ASTC texture to RGBA8 and encodes it to BC
/// on its first upload, with a submit per texture: zone loads spend hundreds of milliseconds
/// there. The result only depends on the guest bytes and the image's shape, so it is kept on disk
/// keyed by a 128-bit hash of both; the next upload of the same texture (this session or a later
/// one) copies the BC blocks straight into the image.
///
/// Storage: two append-only pack files per title (current and previous generation). When the
/// current one passes half the size limit it becomes the previous one and the oldest is dropped;
/// a hit in the previous generation is written again into the current one (an approximate LRU
/// with no per-entry bookkeeping). Records carry their key, sizes and a payload hash; a damaged
/// or truncated tail is cut off on open.
///
/// LICHII_ASTC_DISK_CACHE=1 turns it on (off by default); LICHII_ASTC_DISK_CACHE_MB sets the size
/// limit (default 2048).
class AstcDiskCache {
public:
    struct Key {
        u64 lo;
        u64 hi;
        bool operator==(const Key&) const = default;
    };

    /// Whether LICHII_ASTC_DISK_CACHE=1.
    [[nodiscard]] static bool Enabled();

    /// Hashes the guest bytes and the shape (`meta`, any bytes describing it).
    [[nodiscard]] static Key MakeKey(std::span<const u8> guest, std::span<const u8> meta);

    AstcDiskCache();
    ~AstcDiskCache();

    AstcDiskCache(const AstcDiskCache&) = delete;
    AstcDiskCache& operator=(const AstcDiskCache&) = delete;

    /// Copies the cached blocks of `key` into `out` (whose size must match what was stored).
    /// Returns false on a miss. Synchronous: one seek and read of a file the OS usually holds.
    [[nodiscard]] bool Lookup(const Key& key, std::span<u8> out);

    /// Queues `data` to be compressed and appended by the writer thread.
    void Store(const Key& key, std::vector<u8> data);

    /// Whether a Store of this many more bytes may be queued now (bounds the queue's memory).
    [[nodiscard]] bool CanQueue(size_t bytes) const;

    /// Logs hits, misses and sizes every 10 s while there is activity.
    void Report(double saved_ms_estimate_per_hit);

private:
    struct KeyHash {
        size_t operator()(const Key& key) const noexcept {
            return static_cast<size_t>(key.lo ^ (key.hi * 0x9E3779B97F4A7C15ULL));
        }
    };
    struct Entry {
        u32 generation; ///< 0 = current file, 1 = previous file
        u64 offset;     ///< of the payload
        u32 raw_size;
        u32 stored_size;
        u32 flags;
        u64 payload_hash;
    };
    struct Job {
        Key key;
        std::vector<u8> data;
    };

    void EnsureOpen();
    void ScanFile(u32 generation, Common::FS::IOFile& file, bool truncate_tail);
    void Rotate();
    void WriterLoop();
    void Append(const Key& key, std::span<const u8> raw);

    std::filesystem::path dir;
    std::filesystem::path current_path;
    std::filesystem::path previous_path;
    u64 limit_bytes;
    bool opened{};
    bool usable{};

    mutable std::mutex mutex; ///< index, files, sizes
    std::unordered_map<Key, Entry, KeyHash> index;
    std::unique_ptr<Common::FS::IOFile> current_write;
    std::unique_ptr<Common::FS::IOFile> current_read;
    std::unique_ptr<Common::FS::IOFile> previous_read;
    u64 current_size{};
    u64 previous_size{};

    mutable std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<Job> queue;
    size_t queued_bytes{};
    bool stopping{};
    std::thread writer;

    // Statistics (GPU thread for lookups, writer thread for stores).
    std::mutex stats_mutex;
    u64 hits{};
    u64 misses{};
    u64 stored{};
    double read_ms{};
    u64 window_hits{};
    u64 window_misses{};
    u64 window_stored{};
    double window_read_ms{};
    std::chrono::steady_clock::time_point window_start{};
};

} // namespace VideoCommon
