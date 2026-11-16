// SPDX-FileCopyrightText: Copyright 2026 lichii-arc
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <system_error>

#include <fmt/format.h>
#include <xxhash.h>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/zstd_compression.h"
#include "video_core/texture_cache/astc_disk_cache.h"

namespace VideoCommon {
namespace {

constexpr u32 RecordMagic = 0x3142414C; // "LAB1"
constexpr u32 FlagZstd = 1;
constexpr std::string_view FileName = "astc_bcn.v1.bin";
constexpr std::string_view PreviousFileName = "astc_bcn.v1.old.bin";
/// Records larger than this are not kept (a 4096x4096 BC3 texture with mips is ~22 MiB).
constexpr size_t MaxRecordBytes = 64ULL << 20;
/// Bytes waiting for the writer thread at most; later stores are dropped until it catches up.
constexpr size_t MaxQueuedBytes = 192ULL << 20;

struct RecordHeader {
    u32 magic;
    u32 flags;
    u64 key_lo;
    u64 key_hi;
    u32 raw_size;
    u32 stored_size;
    u64 payload_hash;
};
static_assert(sizeof(RecordHeader) == 40);

u64 EnvU64(const char* name, u64 fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    char* end{};
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    return end != value ? static_cast<u64>(parsed) : fallback;
}

} // Anonymous namespace

bool AstcDiskCache::Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("LICHII_ASTC_DISK_CACHE");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

AstcDiskCache::Key AstcDiskCache::MakeKey(std::span<const u8> guest, std::span<const u8> meta) {
    const u64 seed = XXH3_64bits(meta.data(), meta.size());
    const XXH128_hash_t hash = XXH3_128bits_withSeed(guest.data(), guest.size(), seed);
    return Key{.lo = hash.low64, .hi = hash.high64};
}

AstcDiskCache::AstcDiskCache()
    : limit_bytes{EnvU64("LICHII_ASTC_DISK_CACHE_MB", 2048) << 20},
      window_start{std::chrono::steady_clock::now()} {
    writer = std::thread([this] { WriterLoop(); });
}

AstcDiskCache::~AstcDiskCache() {
    {
        std::scoped_lock lock{queue_mutex};
        stopping = true;
    }
    queue_cv.notify_all();
    if (writer.joinable()) {
        writer.join();
    }
}

void AstcDiskCache::EnsureOpen() {
    // Called with `mutex` held.
    if (opened) {
        return;
    }
    const u64 program_id = Settings::GetCurrentProgramID();
    if (program_id == 0) {
        return; // Not known yet: try again on the next use.
    }
    opened = true;
    dir = Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir) /
          fmt::format("{:016x}", program_id) / "astc_bcn";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    current_path = dir / FileName;
    previous_path = dir / PreviousFileName;

    if (std::filesystem::exists(previous_path, ec)) {
        previous_read = std::make_unique<Common::FS::IOFile>(
            previous_path, Common::FS::FileAccessMode::Read, Common::FS::FileType::BinaryFile,
            Common::FS::FileShareFlag::ShareReadWrite);
        if (previous_read->IsOpen()) {
            ScanFile(1, *previous_read, false);
            previous_size = previous_read->GetSize();
        } else {
            previous_read.reset();
        }
    }
    {
        // Scan (and cut a damaged tail off) the current file before appending to it.
        Common::FS::IOFile scan(current_path, Common::FS::FileAccessMode::ReadAppend,
                                Common::FS::FileType::BinaryFile,
                                Common::FS::FileShareFlag::ShareReadWrite);
        if (scan.IsOpen()) {
            ScanFile(0, scan, true);
        }
    }
    current_write = std::make_unique<Common::FS::IOFile>(
        current_path, Common::FS::FileAccessMode::Append, Common::FS::FileType::BinaryFile,
        Common::FS::FileShareFlag::ShareReadWrite);
    current_read = std::make_unique<Common::FS::IOFile>(
        current_path, Common::FS::FileAccessMode::Read, Common::FS::FileType::BinaryFile,
        Common::FS::FileShareFlag::ShareReadWrite);
    usable = current_write->IsOpen() && current_read->IsOpen();
    current_size = usable ? current_write->GetSize() : 0;
    LOG_INFO(Render_Vulkan,
             "ASTC disk cache: {} entries, {} MiB in use of {} MiB, at {}{}", index.size(),
             (current_size + previous_size) >> 20, limit_bytes >> 20, dir.string(),
             usable ? "" : " (could not open: off)");
}

void AstcDiskCache::ScanFile(u32 generation, Common::FS::IOFile& file, bool truncate_tail) {
    const u64 size = file.GetSize();
    u64 offset = 0;
    std::vector<u8> payload;
    while (offset + sizeof(RecordHeader) <= size) {
        RecordHeader header{};
        if (!file.Seek(static_cast<s64>(offset)) || !file.ReadObject(header) ||
            header.magic != RecordMagic || header.stored_size == 0 ||
            header.stored_size > MaxRecordBytes || header.raw_size > MaxRecordBytes ||
            offset + sizeof(RecordHeader) + header.stored_size > size) {
            break;
        }
        index.insert_or_assign(Key{header.key_lo, header.key_hi},
                               Entry{
                                   .generation = generation,
                                   .offset = offset + sizeof(RecordHeader),
                                   .raw_size = header.raw_size,
                                   .stored_size = header.stored_size,
                                   .flags = header.flags,
                                   .payload_hash = header.payload_hash,
                               });
        offset += sizeof(RecordHeader) + header.stored_size;
    }
    if (truncate_tail && offset != size) {
        LOG_WARNING(Render_Vulkan, "ASTC disk cache: cutting {} damaged bytes off {}",
                    size - offset, file.GetPath().string());
        (void)file.SetSize(offset);
    }
}

bool AstcDiskCache::CanQueue(size_t bytes) const {
    if (bytes == 0 || bytes > MaxRecordBytes) {
        return false;
    }
    std::scoped_lock lock{queue_mutex};
    return queued_bytes + bytes <= MaxQueuedBytes;
}

bool AstcDiskCache::Lookup(const Key& key, std::span<u8> out) {
    const auto begin = std::chrono::steady_clock::now();
    bool hit = false;
    bool from_previous = false;
    {
        std::scoped_lock lock{mutex};
        EnsureOpen();
        if (usable) {
            const auto it = index.find(key);
            if (it != index.end() && it->second.raw_size == out.size()) {
                const Entry entry = it->second;
                Common::FS::IOFile* const file =
                    entry.generation == 0 ? current_read.get() : previous_read.get();
                std::vector<u8> stored(entry.stored_size);
                if (file != nullptr && file->Seek(static_cast<s64>(entry.offset)) &&
                    file->ReadSpan(std::span<u8>(stored)) == stored.size() &&
                    XXH3_64bits(stored.data(), stored.size()) == entry.payload_hash) {
                    if ((entry.flags & FlagZstd) != 0) {
                        hit = Common::Compression::DecompressDataZSTD(stored, out) == out.size();
                    } else if (stored.size() == out.size()) {
                        std::memcpy(out.data(), stored.data(), out.size());
                        hit = true;
                    }
                }
                if (!hit) {
                    index.erase(it); // Damaged: decode again and store it anew.
                }
                from_previous = hit && entry.generation != 0;
            }
        }
    }
    if (from_previous && CanQueue(out.size())) {
        // Keep it past the next rotation.
        Store(key, std::vector<u8>(out.begin(), out.end()));
    }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin)
            .count();
    std::scoped_lock lock{stats_mutex};
    if (hit) {
        ++hits;
        ++window_hits;
        read_ms += ms;
        window_read_ms += ms;
    } else {
        ++misses;
        ++window_misses;
    }
    return hit;
}

void AstcDiskCache::Store(const Key& key, std::vector<u8> data) {
    if (data.empty() || data.size() > MaxRecordBytes) {
        return;
    }
    {
        std::scoped_lock lock{queue_mutex};
        if (queued_bytes + data.size() > MaxQueuedBytes) {
            return;
        }
        queued_bytes += data.size();
        queue.push_back(Job{.key = key, .data = std::move(data)});
    }
    queue_cv.notify_one();
}

void AstcDiskCache::WriterLoop() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock{queue_mutex};
            queue_cv.wait(lock, [this] { return stopping || !queue.empty(); });
            if (queue.empty()) {
                return; // stopping, and everything queued is written
            }
            job = std::move(queue.front());
            queue.pop_front();
            queued_bytes -= job.data.size();
        }
        Append(job.key, job.data);
    }
}

void AstcDiskCache::Append(const Key& key, std::span<const u8> raw) {
    // Level 3: BC blocks shrink a little, and the writer must keep up with zone loads.
    std::vector<u8> compressed = Common::Compression::CompressDataZSTD(raw.data(), raw.size(), 3);
    const bool use_zstd = !compressed.empty() && compressed.size() < raw.size();
    const std::span<const u8> payload =
        use_zstd ? std::span<const u8>(compressed) : std::span<const u8>(raw);
    const RecordHeader header{
        .magic = RecordMagic,
        .flags = use_zstd ? FlagZstd : 0,
        .key_lo = key.lo,
        .key_hi = key.hi,
        .raw_size = static_cast<u32>(raw.size()),
        .stored_size = static_cast<u32>(payload.size()),
        .payload_hash = XXH3_64bits(payload.data(), payload.size()),
    };
    std::scoped_lock lock{mutex};
    EnsureOpen();
    if (!usable) {
        return;
    }
    if (const auto it = index.find(key); it != index.end() && it->second.generation == 0) {
        return; // Already in the current file.
    }
    if (current_size + sizeof(header) + payload.size() > limit_bytes / 2) {
        Rotate();
        if (!usable) {
            return;
        }
    }
    const u64 offset = current_size;
    if (!current_write->WriteObject(header) ||
        current_write->WriteSpan(payload) != payload.size() || !current_write->Flush()) {
        LOG_WARNING(Render_Vulkan, "ASTC disk cache: write failed; turning it off");
        usable = false;
        return;
    }
    current_size += sizeof(header) + payload.size();
    index.insert_or_assign(key, Entry{
                                    .generation = 0,
                                    .offset = offset + sizeof(header),
                                    .raw_size = header.raw_size,
                                    .stored_size = header.stored_size,
                                    .flags = header.flags,
                                    .payload_hash = header.payload_hash,
                                });
    std::scoped_lock stats_lock{stats_mutex};
    ++stored;
    ++window_stored;
}

void AstcDiskCache::Rotate() {
    // Called with `mutex` held. The current file becomes the previous one.
    current_write.reset();
    current_read.reset();
    previous_read.reset();
    std::error_code ec;
    std::filesystem::remove(previous_path, ec);
    std::filesystem::rename(current_path, previous_path, ec);
    std::erase_if(index, [](const auto& item) { return item.second.generation == 1; });
    for (auto& [key, entry] : index) {
        entry.generation = 1;
    }
    previous_read = std::make_unique<Common::FS::IOFile>(
        previous_path, Common::FS::FileAccessMode::Read, Common::FS::FileType::BinaryFile,
        Common::FS::FileShareFlag::ShareReadWrite);
    if (!previous_read->IsOpen()) {
        previous_read.reset();
        std::erase_if(index, [](const auto& item) { return item.second.generation == 1; });
    }
    previous_size = previous_read ? previous_read->GetSize() : 0;
    current_write = std::make_unique<Common::FS::IOFile>(
        current_path, Common::FS::FileAccessMode::Write, Common::FS::FileType::BinaryFile,
        Common::FS::FileShareFlag::ShareReadWrite);
    current_read = std::make_unique<Common::FS::IOFile>(
        current_path, Common::FS::FileAccessMode::Read, Common::FS::FileType::BinaryFile,
        Common::FS::FileShareFlag::ShareReadWrite);
    usable = current_write->IsOpen() && current_read->IsOpen();
    current_size = 0;
    LOG_INFO(Render_Vulkan, "ASTC disk cache: rotated ({} MiB kept as the previous generation)",
             previous_size >> 20);
}

void AstcDiskCache::Report(double saved_ms_estimate_per_hit) {
    const auto now = std::chrono::steady_clock::now();
    u64 h{}, m{}, s{};
    double ms{};
    {
        std::scoped_lock lock{stats_mutex};
        if (now - window_start < std::chrono::seconds{10}) {
            return;
        }
        h = window_hits;
        m = window_misses;
        s = window_stored;
        ms = window_read_ms;
        window_hits = window_misses = window_stored = 0;
        window_read_ms = 0;
        window_start = now;
    }
    if (h == 0 && m == 0 && s == 0) {
        return;
    }
    u64 disk{};
    size_t entries{};
    {
        std::scoped_lock lock{mutex};
        disk = current_size + previous_size;
        entries = index.size();
    }
    LOG_INFO(Render_Vulkan,
             "ASTC disk cache in 10 s: {} hits ({:.1f} ms reading, ~{:.0f} ms of transcodes "
             "saved), {} misses, {} stored; {} entries, {} MiB on disk",
             h, ms, saved_ms_estimate_per_hit * static_cast<double>(h), m, s, entries,
             disk >> 20);
}

} // namespace VideoCommon
