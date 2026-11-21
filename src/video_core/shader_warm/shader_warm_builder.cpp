// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>

#include "common/cityhash.h"
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/file_sys/nup_container.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/loader/loader.h"
#include "video_core/shader_warm/emission_stamp.h"
#include "video_core/pipeline_cache_files.h"
#include "video_core/shader_warm/shader_archives.h"
#include "video_core/shader_warm/shader_warm_builder.h"
#include "video_core/shader_warm/warm_records.h"

namespace VideoCommon::ShaderWarm {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view StageReading = "Reading game files";
constexpr std::string_view StageExtracting = "Extracting shaders";
constexpr std::string_view StageConverting = "Converting";
constexpr std::string_view StageWriting = "Writing";
constexpr std::string_view StagePacks = "Reading asset packs (MiB)";
/// Conversions in flight (each holds a copy of its program).
constexpr size_t MaxPendingJobs = 2048;
constexpr u32 PrecachedPipelinesMagic = 0x32504750; // "PGP2", with the graphics calls mode

fs::path TitleDir(u64 title_id) {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir) /
           fmt::format("{:016x}", title_id);
}

struct ArchiveFile {
    std::string path; ///< Inside the RomFS, '/' separated (the archive identity)
    FileSys::VirtualFile file;
};

/// Appends every file under `dir` to `out`, recursively, with its '/' separated path.
void ListFiles(const FileSys::VirtualDir& dir, const std::string& prefix,
               std::vector<ArchiveFile>& out) {
    for (const auto& file : dir->GetFiles()) {
        out.push_back({prefix + file->GetName(), file});
    }
    for (const auto& sub : dir->GetSubdirectories()) {
        ListFiles(sub, prefix + sub->GetName() + "/", out);
    }
}

/// The game's RomFS trees in the order the offline pipeline read them: a .nup gives its base
/// and newest update package (both are read, like the exported packages 0 and 1); other
/// formats give the loader's RomFS.
struct GameFiles {
    std::unique_ptr<FileSys::NupContainer> nup;
    std::unique_ptr<Loader::AppLoader> loader;
    std::vector<FileSys::VirtualDir> romfs;
};

/// Opens the game file and collects its RomFS trees into `game`. Returns the error text when the
/// file cannot be opened or has no RomFS.
std::optional<std::string> OpenGame(Core::System& system, const fs::path& game_path,
                                    GameFiles& game) {
    FileSys::VirtualFilesystem filesystem = system.GetFilesystem();
    if (filesystem == nullptr) {
        filesystem = std::make_shared<FileSys::RealVfsFilesystem>();
    }
    const auto file =
        filesystem->OpenFile(Common::FS::PathToUTF8String(game_path), FileSys::OpenMode::Read);
    if (file == nullptr) {
        return "Cannot open the game file";
    }
    if (FileSys::NupContainer::IsNupFile(file)) {
        // Small memory cache: every member is read once.
        game.nup = std::make_unique<FileSys::NupContainer>(file, 128 * 1024 * 1024);
        if (game.nup->GetStatus() != Loader::ResultStatus::Success) {
            return "The game file is not a valid package";
        }
        std::vector<size_t> packages;
        if (const auto base = game.nup->FindBasePackage()) {
            packages.push_back(*base);
        }
        if (const auto update = game.nup->FindBestUpdatePackage()) {
            packages.push_back(*update);
        }
        std::sort(packages.begin(), packages.end());
        for (const size_t package : packages) {
            if (auto dir = game.nup->GetRomFSDir(package)) {
                game.romfs.push_back(std::move(dir));
            }
        }
    } else {
        game.loader = Loader::GetLoader(system, file);
        if (game.loader == nullptr) {
            return "Unsupported game file format";
        }
        FileSys::VirtualFile romfs;
        if (game.loader->ReadRomFS(romfs) != Loader::ResultStatus::Success || romfs == nullptr) {
            return "Cannot read the game's RomFS (are the keys installed?)";
        }
        if (auto dir = FileSys::ExtractRomFS(romfs)) {
            game.romfs.push_back(std::move(dir));
        }
    }
    if (game.romfs.empty()) {
        return "The game has no RomFS";
    }
    return std::nullopt;
}

struct PairHash {
    u128 a{};
    u128 b{};
    bool operator==(const PairHash&) const = default;
};
struct PairHasher {
    size_t operator()(const PairHash& h) const noexcept {
        return static_cast<size_t>(h.a[0] ^ (h.b[0] * 0x9E3779B97F4A7C15ULL));
    }
};
struct U128Hasher {
    size_t operator()(const u128& h) const noexcept {
        return static_cast<size_t>(h[0]);
    }
};

u128 Hash128(std::span<const u8> bytes) {
    return Common::CityHash128(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

/// One unique vertex or fragment program (make_graphics_warm.py: first manifest program per
/// code hash, with the origins of that program's code + control identity).
struct GraphicsProgram {
    u32 stage{};
    u128 control{};
    std::vector<u32> archives;
    bool built{};
    u64 hash{};
    u128 environment{};
};

struct Job {
    size_t seq{};
    bool graphics{};
    u32 stage{};
    size_t graphics_index{};
    std::vector<u8> program;
    std::vector<u8> constants;
    ComputeControl control{};
    bool guessed{}; ///< lichii-arc: a workgroup size guessed for a program without one
};

struct JobResult {
    bool ok{};
    std::vector<u8> bytes; ///< Compute: full entry. Graphics: env record.
    ComputeKey compute_key{};
    u64 hash{};
};

class Converter {
public:
    /// Starts `threads` workers that convert pushed jobs until `stop` is requested.
    Converter(size_t threads, std::stop_token stop_) : stop{std::move(stop_)} {
        for (size_t i = 0; i < threads; ++i) {
            workers.emplace_back([this] { Work(); });
        }
    }

    /// Lets the workers finish the queued jobs, then joins them.
    ~Converter() {
        {
            std::scoped_lock lock{mutex};
            closing = true;
        }
        jobs_cv.notify_all();
        for (auto& worker : workers) {
            worker.join();
        }
    }

    void Push(Job job) {
        {
            std::scoped_lock lock{mutex};
            jobs.push_back(std::move(job));
            ++pushed;
        }
        jobs_cv.notify_one();
    }

    /// Next result in submission order; waits when `wait` is set, else empty when not ready.
    std::optional<JobResult> Next(bool wait) {
        std::unique_lock lock{mutex};
        for (;;) {
            const auto it = results.find(next);
            if (it != results.end()) {
                JobResult result = std::move(it->second);
                results.erase(it);
                ++next;
                return result;
            }
            if (!wait || next == pushed || stop.stop_requested()) {
                return std::nullopt;
            }
            results_cv.wait_for(lock, std::chrono::milliseconds{100});
        }
    }

    size_t Pending() {
        std::scoped_lock lock{mutex};
        return pushed - next;
    }

private:
    /// Worker loop: takes jobs in order, converts each one (skipped once stopping) and files the
    /// result under its sequence number. Returns once closing with no job left.
    void Work() {
        Common::SetCurrentThreadName("ShaderWarmBuild");
        Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
        for (;;) {
            Job job;
            {
                std::unique_lock lock{mutex};
                jobs_cv.wait(lock, [this] { return closing || !jobs.empty(); });
                if (jobs.empty()) {
                    return;
                }
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            JobResult result;
            if (!stop.stop_requested()) {
                result = Convert(job);
            }
            {
                std::scoped_lock lock{mutex};
                results.emplace(job.seq, std::move(result));
            }
            results_cv.notify_all();
        }
    }

    /// Lowers one program and makes its compute entry or graphics env record. The result is not ok
    /// when lowering or conversion failed.
    static JobResult Convert(const Job& job) {
        JobResult result;
        try {
            LoweredCode lowered;
            if (const auto error = LowerProgram(job.program, job.graphics, lowered)) {
                static std::atomic<u32> logged{};
                if (WarmComputeSkippedEnabled() && !job.graphics &&
                    logged.fetch_add(1, std::memory_order_relaxed) < 20) {
                    LOG_INFO(Render_Vulkan, "Shader precache: compute program not converted: {}",
                             *error);
                } else {
                    LOG_DEBUG(Render_Vulkan, "Shader precache: program not converted: {}", *error);
                }
                return result;
            }
            if (job.graphics) {
                result.hash = StageHash(lowered.code);
                result.bytes = GraphicsEnv(job.stage, lowered, job.program, job.constants);
            } else {
                result.bytes =
                    ComputeEntry(lowered, job.control, job.constants, result.compute_key);
            }
            result.ok = true;
        } catch (const std::exception& e) {
            static std::atomic<u32> logged{};
            if (WarmComputeSkippedEnabled() && !job.graphics &&
                logged.fetch_add(1, std::memory_order_relaxed) < 20) {
                LOG_INFO(Render_Vulkan, "Shader precache: compute conversion failed: {}",
                         e.what());
            } else {
                LOG_DEBUG(Render_Vulkan, "Shader precache: conversion failed: {}", e.what());
            }
        }
        return result;
    }

    std::stop_token stop;
    std::mutex mutex;
    std::condition_variable jobs_cv;
    std::condition_variable results_cv;
    std::deque<Job> jobs;
    std::map<size_t, JobResult> results;
    size_t pushed{};
    size_t next{};
    bool closing{};
    std::vector<std::thread> workers;
};

template <typename T>
void Write(std::ofstream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

void WriteHeader(std::ofstream& out) {
    out.write("yuzucach", 8);
    Write(out, WarmCacheVersion);
}

std::atomic<bool> warm_before_start{false};
std::atomic<bool> skip_warm{false};

/// Records `entries` for the finished precache file at `warm_file` (see WarmCountPath).
void WriteWarmCount(const fs::path& warm_file, u64 entries) {
    std::error_code ec;
    const auto size = fs::file_size(warm_file, ec);
    if (ec) {
        return;
    }
    std::ofstream out(WarmCountPath(warm_file), std::ios::trunc);
    out << entries << ' ' << size << ' ' << CurrentWarmBuilderVersion() << '\n';
}

} // Anonymous namespace

fs::path ComputeWarmPath(u64 title_id) {
    return TitleDir(title_id) / "warm-compute.bin";
}

fs::path GraphicsWarmPath(u64 title_id) {
    return TitleDir(title_id) / "warm-graphics.bin";
}

fs::path WarmMarkerPath(const fs::path& warm_file) {
    fs::path marker = warm_file;
    marker.replace_extension(".done");
    return marker;
}

fs::path WarmRetryPath(const fs::path& warm_file) {
    fs::path retry = warm_file;
    retry.replace_filename(warm_file.stem().string() + "-retry" + warm_file.extension().string());
    return retry;
}

fs::path WarmCountPath(const fs::path& warm_file) {
    fs::path count = warm_file;
    count.replace_extension(".count");
    return count;
}

u64 ReadWarmCount(const fs::path& warm_file) {
    std::ifstream in(WarmCountPath(warm_file));
    u64 entries{};
    u64 size{};
    if (!(in >> entries >> size)) {
        return 0;
    }
    std::error_code ec;
    const auto actual = fs::file_size(warm_file, ec);
    return !ec && actual == size ? entries : 0;
}

u64 EmissionStamp() {
    return GeneratedEmissionStamp;
}

std::optional<WarmStamp> CurrentWarmStamp(const fs::path& warm_file, u64 entries, u32 driver,
                                          u32 device_id, u32 cache_version, u64 variant) {
    std::error_code ec;
    const auto size = fs::file_size(warm_file, ec);
    if (ec) {
        return std::nullopt;
    }
    // Hashed in chunks (the file is ~70 MB): tens of milliseconds at boot.
    std::ifstream in(warm_file, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::vector<char> chunk(8u << 20);
    u64 hash = size;
    u64 read_total = 0;
    while (in) {
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const auto got = static_cast<size_t>(in.gcount());
        if (got == 0) {
            break;
        }
        hash = Common::CityHash64WithSeed(chunk.data(), got, hash);
        read_total += got;
    }
    if (read_total != size) {
        return std::nullopt;
    }
    return WarmStamp{.size = size,
                     .content_hash = hash,
                     .entries = entries,
                     .driver = driver,
                     .device_id = device_id,
                     .cache_version = cache_version,
                     .variant = variant};
}

std::optional<WarmStamp> ReadWarmMarker(const fs::path& warm_file) {
    std::ifstream in(WarmMarkerPath(warm_file));
    WarmStamp stamp;
    // A marker of the older format (last-write time, no variant) fails here: the compute
    // programs not in the cache are built once more.
    std::string format;
    if (!(in >> format) || format != "v2" ||
        !(in >> stamp.size >> stamp.content_hash >> stamp.entries >> stamp.driver >>
          stamp.device_id >> stamp.cache_version >> stamp.variant)) {
        return std::nullopt;
    }
    return stamp;
}

bool WriteWarmMarker(const fs::path& warm_file, const WarmStamp& stamp) {
    std::ofstream out(WarmMarkerPath(warm_file), std::ios::trunc);
    out << "v2 " << stamp.size << ' ' << stamp.content_hash << ' ' << stamp.entries << ' '
        << stamp.driver << ' ' << stamp.device_id << ' ' << stamp.cache_version << ' '
        << stamp.variant << '\n';
    out.close();
    return static_cast<bool>(out);
}

void DropStaleWarmMarker(const fs::path& warm_file, const fs::path& new_file) {
    if (const auto marker = ReadWarmMarker(warm_file)) {
        const auto incoming = CurrentWarmStamp(new_file, 0, 0, 0, 0);
        if (incoming && incoming->size == marker->size &&
            incoming->content_hash == marker->content_hash) {
            return;
        }
    }
    std::error_code ignored;
    fs::remove(WarmMarkerPath(warm_file), ignored);
}

fs::path WarmProgressPath(const fs::path& warm_file) {
    fs::path progress = warm_file;
    progress.replace_extension(".progress");
    return progress;
}

std::unordered_set<u64> ReadWarmProgress(const fs::path& warm_file, const WarmStamp& current) {
    std::ifstream in(WarmProgressPath(warm_file));
    WarmStamp stamp;
    std::string format;
    if (!(in >> format) || format != "v1" ||
        !(in >> stamp.size >> stamp.content_hash >> stamp.entries >> stamp.driver >>
          stamp.device_id >> stamp.cache_version >> stamp.variant) ||
        !stamp.Matches(current)) {
        return {};
    }
    std::unordered_set<u64> built;
    u64 key{};
    while (in >> std::hex >> key) {
        built.insert(key);
    }
    return built;
}

bool WriteWarmProgress(const fs::path& warm_file, const WarmStamp& stamp,
                       const std::unordered_set<u64>& built) {
    std::ofstream out(WarmProgressPath(warm_file), std::ios::trunc);
    out << "v1 " << stamp.size << ' ' << stamp.content_hash << ' ' << stamp.entries << ' '
        << stamp.driver << ' ' << stamp.device_id << ' ' << stamp.cache_version << ' '
        << stamp.variant << '\n'
        << std::hex;
    for (const u64 key : built) {
        out << key << '\n';
    }
    out.close();
    return static_cast<bool>(out);
}

fs::path WarmRequestPath(const fs::path& warm_file) {
    fs::path request = warm_file;
    request.replace_extension(".requested");
    return request;
}

bool WarmRequested(const fs::path& warm_file) {
    std::error_code ec;
    return fs::exists(WarmRequestPath(warm_file), ec);
}

bool SetWarmRequested(const fs::path& warm_file, bool requested) {
    std::error_code ec;
    if (!requested) {
        fs::remove(WarmRequestPath(warm_file), ec);
        return !ec;
    }
    fs::create_directories(warm_file.parent_path(), ec);
    std::ofstream out(WarmRequestPath(warm_file), std::ios::trunc);
    return static_cast<bool>(out);
}

bool WarmPending(u64 title_id) {
    const auto file = ComputeWarmPath(title_id);
    std::error_code ec;
    return !fs::exists(file, ec) || !fs::exists(WarmMarkerPath(file), ec);
}

void RequestWarmBeforeStart(bool before_start) {
    skip_warm.store(false, std::memory_order_relaxed);
    warm_before_start.store(before_start, std::memory_order_relaxed);
}

bool TakeWarmBeforeStart() {
    return warm_before_start.exchange(false, std::memory_order_relaxed);
}

void RequestSkipWarm() {
    skip_warm.store(true, std::memory_order_relaxed);
}

bool TakeSkipWarm() {
    return skip_warm.exchange(false, std::memory_order_relaxed);
}

fs::path PrecachedPipelinesPath(u64 title_id) {
    return TitleDir(title_id) / "precache-graphics.bin";
}

std::unordered_set<u64> ReadPrecachedPipelines(const fs::path& file, const DriverStamp& stamp) {
    std::unordered_set<u64> hashes;
    std::ifstream in(file, std::ios::binary);
    u32 magic{};
    DriverStamp saved;
    u64 count{};
    if (!in.read(reinterpret_cast<char*>(&magic), sizeof(magic)) ||
        magic != PrecachedPipelinesMagic ||
        !in.read(reinterpret_cast<char*>(&saved.driver), sizeof(saved.driver)) ||
        !in.read(reinterpret_cast<char*>(&saved.device_id), sizeof(saved.device_id)) ||
        !in.read(reinterpret_cast<char*>(&saved.cache_version), sizeof(saved.cache_version)) ||
        !in.read(reinterpret_cast<char*>(&saved.graphics_no_inline),
                 sizeof(saved.graphics_no_inline)) ||
        !in.read(reinterpret_cast<char*>(&count), sizeof(count)) || saved != stamp) {
        return hashes;
    }
    for (u64 i = 0; i < count; ++i) {
        u64 hash{};
        if (!in.read(reinterpret_cast<char*>(&hash), sizeof(hash))) {
            break;
        }
        hashes.insert(hash);
    }
    return hashes;
}

bool WritePrecachedPipelines(const fs::path& file, const DriverStamp& stamp,
                             const std::unordered_set<u64>& hashes) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    Write(out, PrecachedPipelinesMagic);
    Write(out, stamp.driver);
    Write(out, stamp.device_id);
    Write(out, stamp.cache_version);
    Write(out, stamp.graphics_no_inline);
    Write(out, static_cast<u64>(hashes.size()));
    for (const u64 hash : hashes) {
        Write(out, hash);
    }
    out.close();
    return static_cast<bool>(out);
}

bool DeleteWarmFiles(u64 title_id, std::string* error) {
    LOG_INFO(Render_Vulkan, "Shader precache: deleting the precache files of {:016x}", title_id);
    bool ok = true;
    const auto remove = [&](const fs::path& path) {
        std::error_code ec;
        fs::remove(path, ec);
        if (ec) {
            ok = false;
            if (error != nullptr) {
                *error = ec.message();
            }
        }
    };
    for (const auto& warm : {ComputeWarmPath(title_id), GraphicsWarmPath(title_id)}) {
        fs::path tmp = warm;
        tmp += ".tmp";
        for (const auto& path : {warm, WarmMarkerPath(warm), WarmCountPath(warm), tmp,
                                 WarmMarkerPath(WarmRetryPath(warm)), WarmProgressPath(warm),
                                 WarmRequestPath(warm)}) {
            remove(path);
        }
    }
    remove(PrecachedPipelinesPath(title_id));
    return ok;
}

std::optional<std::string> WarmFilesStale(const fs::path& warm_file) {
    std::error_code ec;
    if (!fs::exists(warm_file, ec)) {
        return "none built yet";
    }
    {
        std::ifstream in(warm_file, std::ios::binary);
        std::array<char, 8> magic{};
        u32 version{};
        in.read(magic.data(), magic.size())
            .read(reinterpret_cast<char*>(&version), sizeof(version));
        const auto& compatible = VideoCommon::CompatibleVulkanPipelineCacheVersions;
        if (!in || magic != VideoCommon::PipelineCacheMagic ||
            (version != WarmCacheVersion &&
             std::ranges::find(compatible, version) == compatible.end())) {
            return "built for another cache version";
        }
    }
    std::ifstream in(WarmCountPath(warm_file));
    u64 entries{};
    u64 size{};
    u32 builder{};
    if (!(in >> entries >> size >> builder) || builder != CurrentWarmBuilderVersion()) {
        return "built by an older version";
    }
    if (fs::file_size(warm_file, ec) != size || ec) {
        return "changed since it was built";
    }
    return std::nullopt;
}

Result BuildWarmFiles(Core::System& system, const fs::path& game_path, u64 title_id,
                      const std::function<void(const Progress&)>& progress,
                      std::stop_token stop) {
    return BuildWarmFilesWith(system, game_path, title_id, BuildOptions{}, progress,
                              std::move(stop));
}

Result BuildWarmFilesWith(Core::System& system, const fs::path& game_path, u64 title_id,
                          const BuildOptions& options,
                          const std::function<void(const Progress&)>& progress,
                          std::stop_token stop) {
    Result result;
    const auto report = [&](std::string_view stage, size_t done, size_t total) {
        if (progress) {
            progress(Progress{std::string(stage), done, total});
        }
    };
    const auto begin = std::chrono::steady_clock::now();
    report(StageReading, 0, 0);

    const fs::path output_dir = options.output_dir.empty() ? TitleDir(title_id) : options.output_dir;
    std::error_code ec;
    fs::create_directories(output_dir, ec);
    const fs::path compute_path = output_dir / "warm-compute.bin";
    const fs::path graphics_path = output_dir / "warm-graphics.bin";
    fs::path compute_tmp = compute_path;
    compute_tmp += ".tmp";
    fs::path graphics_tmp = graphics_path;
    graphics_tmp += ".tmp";
    std::ofstream compute_out;
    std::ofstream graphics_out;
    const auto fail = [&](std::string error) {
        compute_out.close();
        graphics_out.close();
        std::error_code ignored;
        fs::remove(compute_tmp, ignored);
        fs::remove(graphics_tmp, ignored);
        result.ok = false;
        result.error = std::move(error);
        LOG_WARNING(Render_Vulkan, "Shader precache build for {:016x} failed: {}", title_id,
                    result.error);
        return result;
    };

    // Draw state guesses.
    std::vector<fs::path> caches;
    if (options.runtime_caches) {
        caches = *options.runtime_caches;
    } else {
        // The title's play history the guesses come from: its v2 log, else an older disk cache
        caches = FindRuntimeCaches(TitleDir(title_id));
    }
    const RuntimeStats stats = ReadRuntimeStats(caches);
    const bool use_stats = stats.occurrences != 0;
    const GraphicsKey vertex_template = stats.templates[0] ? *stats.templates[0]
                                                           : DefaultTemplate(true);
    const GraphicsKey fragment_template = stats.templates[1] ? *stats.templates[1]
                                                             : DefaultTemplate(false);
    LOG_INFO(Render_Vulkan,
             "Shader precache build for {:016x}: {} runtime stage records from {} cache(s){}",
             title_id, stats.occurrences, caches.size(),
             use_stats ? "" : ", built-in draw state defaults");

    GameFiles game;
    if (const auto error = OpenGame(system, game_path, game)) {
        return fail(*error);
    }

    // Archive list, in the offline pipeline's order: every shader archive of each package, then
    // each package's effects, sorted by path inside a package (see ClassifyGameFile).
    std::vector<ArchiveFile> archives;
    std::vector<ArchiveFile> effects;
    std::vector<ArchiveFile> packs;
    ZstdDictionaries dictionaries;
    FileSys::VirtualFile dictionary_pack;
    for (const auto& romfs : game.romfs) {
        std::vector<ArchiveFile> files;
        ListFiles(romfs, "", files);
        std::sort(files.begin(), files.end(),
                  [](const ArchiveFile& a, const ArchiveFile& b) { return a.path < b.path; });
        std::vector<ArchiveFile> package_effects;
        for (auto& file : files) {
            switch (ClassifyGameFile(file.path)) {
            case GameFileKind::ShaderArchive:
                archives.push_back(file);
                break;
            case GameFileKind::Effect:
                package_effects.push_back(file);
                break;
            case GameFileKind::ZstdDictionaries:
                dictionary_pack = file.file; // the newest package wins
                break;
            case GameFileKind::Other:
                if (IsFramePack(file.file->ReadBytes(4, 0), file.path, file.file->GetSize())) {
                    packs.push_back(file);
                }
                break;
            }
        }
        effects.insert(effects.end(), package_effects.begin(), package_effects.end());
    }
    archives.insert(archives.end(), effects.begin(), effects.end());
    if (dictionary_pack) {
        try {
            dictionaries.Load(dictionary_pack->ReadAllBytes());
        } catch (const ArchiveError& e) {
            LOG_WARNING(Render_Vulkan, "Shader precache: zstd dictionary pack: {}", e.what());
        }
    }
    // Without archives the files are written empty: the precache then has only the pipeline
    // cache of earlier sessions to build.
    result.archives = archives.size();
    result.packs = packs.size();
    LOG_INFO(Render_Vulkan,
             "Shader precache build: {} archives, {} asset packs, {} zstd dictionaries",
             archives.size(), packs.size(), dictionaries.Count());

    compute_out.open(compute_tmp, std::ios::binary | std::ios::trunc);
    graphics_out.open(graphics_tmp, std::ios::binary | std::ios::trunc);
    if (!compute_out || !graphics_out) {
        return fail("Cannot write to " + Common::FS::PathToUTF8String(output_dir));
    }
    WriteHeader(compute_out);
    WriteHeader(graphics_out);

    // Bookkeeping in program order (the order the manifest lists programs).
    std::unordered_set<PairHash, PairHasher> compute_identities;
    std::unordered_map<u128, size_t, U128Hasher> graphics_index; // code -> program
    std::vector<GraphicsProgram> graphics;
    std::unordered_set<u128, U128Hasher> compute_keys;
    std::unordered_set<u128, U128Hasher> fragment_hashes;
    std::unordered_set<u128, U128Hasher> vertex_hashes;
    struct WrittenVertex {
        size_t program;
        u64 key_offset;
    };
    std::vector<WrittenVertex> written_vertices;
    struct HeldVertex {
        size_t program;
        std::vector<u8> env;
    };
    std::vector<HeldVertex> held_vertices;
    // lichii-arc (LICHII_WARM_TESS): the vertex and tessellation evaluation programs of each BNSH
    // variation with a tessellation stage, written last as one entry each so the warm-up builds
    // their pre-rasterization library, which a vertex-only entry cannot.
    const bool warm_tess = WarmTessellationEnabled() && stats.templates[2].has_value();
    struct TessGroup {
        std::optional<size_t> vertex;
        std::optional<size_t> tess;
    };
    std::map<u64, TessGroup> tess_groups;
    std::unordered_map<size_t, std::vector<u8>> tess_envs;
    std::unordered_map<u128, std::pair<u64, u64>, U128Hasher> vertex_env_spans;
    size_t archive_errors = 0;
    size_t member_errors = 0;
    size_t seq = 0;
    // lichii-arc LICHII_WARM_COMPUTE_SKIPPED: what the precache leaves out, and the programs
    // without a workgroup size converted later with the likely ones.
    const bool warm_skipped = WarmComputeSkippedEnabled();
    struct DeferredCompute {
        std::vector<u8> program;
        std::vector<u8> constants;
        ComputeControl control;
    };
    std::vector<DeferredCompute> deferred_compute;
    std::map<std::array<u32, 3>, size_t> workgroup_counts;
    std::map<std::string, size_t> member_error_kinds;
    size_t skipped_no_workgroup = 0;
    size_t skipped_short_control = 0;
    size_t skipped_compute_conversion = 0;
    size_t skipped_graphics_conversion = 0;
    size_t guessed_compute = 0;
    std::vector<std::array<u8, 24>> compute_index;
    std::vector<bool> compute_index_guessed;

    const size_t threads =
        options.threads != 0 ? options.threads
                             : std::max<size_t>(std::thread::hardware_concurrency(), 1);
    auto converter = std::make_unique<Converter>(threads, stop);

    // Consumes finished conversions in submission order (first program wins on equal keys).
    std::vector<std::pair<bool, size_t>> job_kinds; // graphics?, graphics program index
    size_t consumed = 0;
    const auto consume = [&](JobResult&& r) {
        const auto [is_graphics, index] = job_kinds[consumed++];
        if (!r.ok) {
            ++result.skipped;
            ++(is_graphics ? skipped_graphics_conversion : skipped_compute_conversion);
            return;
        }
        if (!is_graphics) {
            const u128 key = Hash128(r.bytes);
            if (compute_keys.insert(key).second) {
                compute_out.write(reinterpret_cast<const char*>(r.bytes.data()),
                                  static_cast<std::streamsize>(r.bytes.size()));
                ++result.compute_programs;
                if (warm_skipped) {
                    std::array<u8, 24> index_key{};
                    std::memcpy(index_key.data(), r.compute_key.data(),
                                std::min(index_key.size(), r.compute_key.size()));
                    compute_index.push_back(index_key);
                    compute_index_guessed.push_back(index == 1);
                    guessed_compute += index == 1 ? 1 : 0;
                }
            }
            return;
        }
        GraphicsProgram& program = graphics[index];
        program.built = true;
        program.hash = r.hash;
        program.environment = Hash128(r.bytes);
        if (program.stage == 2) {
            tess_envs.try_emplace(index, std::move(r.bytes));
            return;
        }
        if (program.stage == 4) {
            if (!fragment_hashes.insert(program.environment).second) {
                return; // same key as an earlier entry
            }
            const auto key = GraphicsEntryKey(4, fragment_template, r.hash, 0);
            Write(graphics_out, u32{1});
            graphics_out.write(reinterpret_cast<const char*>(r.bytes.data()),
                               static_cast<std::streamsize>(r.bytes.size()));
            graphics_out.write(reinterpret_cast<const char*>(key.data()),
                               static_cast<std::streamsize>(key.size()));
            ++result.fragment_programs;
            return;
        }
        if (!vertex_hashes.insert(program.environment).second) {
            // Same stage hash: same key only if the attribute guess matches, known at the end.
            held_vertices.push_back({index, std::move(r.bytes)});
            return;
        }
        const auto key = GraphicsEntryKey(0, vertex_template, r.hash, 0);
        Write(graphics_out, u32{1});
        if (warm_tess) {
            vertex_env_spans.try_emplace(program.environment,
                                         static_cast<u64>(graphics_out.tellp()),
                                         static_cast<u64>(r.bytes.size()));
        }
        graphics_out.write(reinterpret_cast<const char*>(r.bytes.data()),
                           static_cast<std::streamsize>(r.bytes.size()));
        const auto key_offset = static_cast<u64>(graphics_out.tellp());
        graphics_out.write(reinterpret_cast<const char*>(key.data()),
                           static_cast<std::streamsize>(key.size()));
        written_vertices.push_back({index, key_offset});
    };
    const auto drain = [&](bool wait_all) {
        while (auto r = converter->Next(wait_all)) {
            consume(std::move(*r));
        }
    };

    // Every program the readers find, from archives and asset packs alike (lichii-arc: graphics
    // programs too, from archives).
    size_t record_archive = 0;
    const auto on_record = [&](const ShaderRecord& record) {
        // Asset packs carry compute programs only here: graphics programs are tied to an archive.
        if (record_archive >= archives.size() && record.stage != ProgramStage::Compute) {
            return;
        }
        const size_t archive_index = record_archive;
        const bool is_tess = warm_tess && record.stage == ProgramStage::TessEval &&
                             record.group != 0;
        const bool is_graphics = record.stage == ProgramStage::Vertex ||
                                 record.stage == ProgramStage::Fragment || is_tess;
        if (record.stage != ProgramStage::Compute && !is_graphics) {
            return;
        }
        const auto note_group = [&](size_t program_index) {
            if (!warm_tess || record.group == 0) {
                return;
            }
            if (record.stage == ProgramStage::TessEval) {
                tess_groups[record.group].tess = program_index;
            } else if (record.stage == ProgramStage::Vertex) {
                tess_groups[record.group].vertex = program_index;
            }
        };
        const std::array program_parts{Hash128(record.code), Hash128(record.constants)};
        const u128 code = Common::CityHash128(
            reinterpret_cast<const char*>(program_parts.data()), sizeof(program_parts));
        const u128 control = Hash128(record.control);
        Job job;
        if (is_graphics) {
            const auto [it, inserted] = graphics_index.try_emplace(code, graphics.size());
            note_group(it->second);
            if (!inserted) {
                GraphicsProgram& program = graphics[it->second];
                if (program.control == control) {
                    program.archives.push_back(static_cast<u32>(archive_index));
                }
                return;
            }
            const u32 stage_number = record.stage == ProgramStage::Vertex   ? 0U
                                     : record.stage == ProgramStage::TessEval ? 2U
                                                                              : 4U;
            graphics.push_back({stage_number, control, {static_cast<u32>(archive_index)}});
            job.graphics = true;
            job.stage = graphics.back().stage;
            job.graphics_index = it->second;
        } else {
            if (!compute_identities.insert(PairHash{code, control}).second) {
                return;
            }
            const auto values = ReadComputeControl(record.control);
            if (!values || !values->workgroup[0] || !values->workgroup[1] ||
                !values->workgroup[2]) {
                if (warm_skipped && values) {
                    // Set by the dispatch at run time: converted after the walk with the
                    // workgroup sizes most common among the game's other programs.
                    ++skipped_no_workgroup;
                    deferred_compute.push_back(
                        {std::vector<u8>(record.code.begin(), record.code.end()),
                         std::vector<u8>(record.constants.begin(), record.constants.end()),
                         *values});
                    return;
                }
                ++(values ? skipped_no_workgroup : skipped_short_control);
                ++result.skipped;
                return;
            }
            if (warm_skipped) {
                ++workgroup_counts[values->workgroup];
            }
            job.control = *values;
            job.stage = 5;
        }
        job.seq = seq++;
        job.program.assign(record.code.begin(), record.code.end());
        job.constants.assign(record.constants.begin(), record.constants.end());
        job_kinds.emplace_back(job.graphics, job.graphics_index);
        converter->Push(std::move(job));
        while (converter->Pending() > MaxPendingJobs && !stop.stop_requested()) {
            if (auto r = converter->Next(true)) {
                consume(std::move(*r));
            }
        }
    };

    for (size_t archive_index = 0; archive_index < archives.size(); ++archive_index) {
        if (stop.stop_requested()) {
            converter.reset();
            return fail("Stopped");
        }
        report(StageExtracting, archive_index, archives.size());
        const auto& archive = archives[archive_index];
        const auto data = archive.file->ReadAllBytes();
        const auto name = archive.path.substr(archive.path.rfind('/') + 1);
        record_archive = archive_index;
        const auto on_member_error = [&](const std::string& error) {
            ++member_errors;
            if (warm_skipped) {
                const size_t colon = error.rfind(": ");
                ++member_error_kinds[colon == std::string::npos ? error : error.substr(colon + 2)];
            }
            LOG_DEBUG(Render_Vulkan, "Shader precache: {}: {}", archive.path, error);
        };
        try {
            WalkArchive(data, name, &dictionaries, on_record, on_member_error);
        } catch (const ArchiveError& e) {
            ++archive_errors;
            LOG_WARNING(Render_Vulkan, "Shader precache: {}: {}", archive.path, e.what());
        }
        drain(false);
    }
    report(StageExtracting, archives.size(), archives.size());

    record_archive = archives.size();
    // Asset packs, read a window at a time: Kirby Air Riders' is 16.6 GB.
    u64 pack_bytes = 0;
    for (const auto& pack : packs) {
        pack_bytes += pack.file->GetSize();
    }
    u64 packs_done = 0;
    FrameScanResult frames;
    for (const auto& pack : packs) {
        const u64 size = pack.file->GetSize();
        const auto read = [&](u64 offset, size_t length) {
            report(StagePacks, static_cast<size_t>((packs_done + offset) >> 20),
                   static_cast<size_t>(pack_bytes >> 20));
            auto bytes = pack.file->ReadBytes(length, static_cast<size_t>(offset));
            drain(false);
            return bytes;
        };
        const size_t compute_before = seq;
        const auto scanned = ScanFrames(size, read, &dictionaries, on_record, stop);
        frames.frames += scanned.frames;
        frames.failed += scanned.failed;
        frames.oversized += scanned.oversized;
        frames.bytes_read += scanned.bytes_read;
        packs_done += size;
        LOG_INFO(Render_Vulkan,
                 "Shader precache: {}: {} zstd frames, {} failed, {} too large, {} new compute "
                 "programs",
                 pack.path, scanned.frames, scanned.failed, scanned.oversized,
                 seq - compute_before);
        if (scanned.stopped) {
            converter.reset();
            return fail("Stopped");
        }
        if (scanned.read_failed) {
            converter.reset();
            return fail(fmt::format("Reading {} failed", pack.path));
        }
    }

    if (warm_skipped && !deferred_compute.empty() && !workgroup_counts.empty()) {
        size_t variants = 4;
        if (const char* value = std::getenv("LICHII_WARM_COMPUTE_SKIPPED_VARIANTS")) {
            variants = std::clamp<size_t>(std::strtoul(value, nullptr, 10), 1, 16);
        }
        std::vector<std::pair<size_t, std::array<u32, 3>>> ranked;
        for (const auto& [size, count] : workgroup_counts) {
            ranked.emplace_back(count, size);
        }
        std::ranges::sort(ranked, std::greater{});
        ranked.resize(std::min(ranked.size(), variants));
        std::string sizes;
        for (const auto& [count, size] : ranked) {
            sizes += fmt::format(" {}x{}x{} ({})", size[0], size[1], size[2], count);
        }
        LOG_INFO(Render_Vulkan,
                 "Shader precache: {} compute programs have no workgroup size in the game files; "
                 "converting each with the most common sizes:{}",
                 deferred_compute.size(), sizes);
        for (const DeferredCompute& program : deferred_compute) {
            for (const auto& [count, size] : ranked) {
                Job job;
                job.control = program.control;
                job.control.workgroup = size;
                job.stage = 5;
                job.guessed = true;
                job.seq = seq++;
                job.program = program.program;
                job.constants = program.constants;
                job_kinds.emplace_back(false, size_t{1});
                converter->Push(std::move(job));
                while (converter->Pending() > MaxPendingJobs && !stop.stop_requested()) {
                    if (auto r = converter->Next(true)) {
                        consume(std::move(*r));
                    }
                }
            }
        }
        deferred_compute.clear();
    }
    const size_t total_jobs = seq;
    while (consumed < total_jobs) {
        if (stop.stop_requested()) {
            converter.reset();
            return fail("Stopped");
        }
        report(StageConverting, consumed, total_jobs);
        if (auto r = converter->Next(true)) {
            consume(std::move(*r));
        }
    }
    report(StageConverting, total_jobs, total_jobs);
    converter.reset();
    for (auto& archive : archives) {
        archive.file = nullptr;
    }
    effects.clear();
    dictionary_pack = nullptr;
    game.romfs.clear();
    game.loader.reset();
    game.nup.reset();

    // Vertex attribute types (make_graphics_warm.py attr_types): per slot, the most common
    // nonzero value among the runtime pipelines of the OTHER vertex shaders of the same
    // archive(s); slots without data are 1 (float). Without runtime data: built-in default.
    std::unordered_map<std::string, u32> archive_ids;
    for (size_t i = 0; i < archives.size(); ++i) {
        archive_ids.try_emplace(archives[i].path, static_cast<u32>(archive_ids.size()));
    }
    const auto archive_id = [&](u32 index) { return archive_ids.at(archives[index].path); };
    // Archive identity: the path inside the RomFS (the same file in both packages is one).
    std::unordered_map<u32, std::set<u64>> by_archive;
    for (const auto& program : graphics) {
        if (program.stage == 0 && program.built &&
            stats.vertex_attributes.contains(program.hash)) {
            for (const u32 a : program.archives) {
                by_archive[archive_id(a)].insert(program.hash);
            }
        }
    }
    const auto attribute_types = [&](const GraphicsProgram& program) -> u64 {
        if (!use_stats) {
            return DefaultAttributeTypes;
        }
        std::set<u64> others;
        for (const u32 a : program.archives) {
            if (const auto it = by_archive.find(archive_id(a)); it != by_archive.end()) {
                others.insert(it->second.begin(), it->second.end());
            }
        }
        others.erase(program.hash);
        std::array<std::array<size_t, 4>, 32> counts{};
        std::array<std::array<size_t, 4>, 32> first{};
        size_t order = 0;
        for (const u64 hash : others) {
            for (const u64 value : stats.vertex_attributes.at(hash)) {
                for (size_t i = 0; i < 32; ++i) {
                    const auto x = static_cast<size_t>((value >> (2 * i)) & 3);
                    if (x != 0) {
                        if (counts[i][x]++ == 0) {
                            first[i][x] = order++;
                        }
                    }
                }
            }
        }
        u64 types = 0;
        for (size_t i = 0; i < 32; ++i) {
            u64 best = 1;
            size_t best_count = 0;
            size_t best_first = 0;
            for (size_t x = 1; x < 4; ++x) {
                if (counts[i][x] > best_count ||
                    (counts[i][x] == best_count && counts[i][x] != 0 && first[i][x] < best_first)) {
                    best = x;
                    best_count = counts[i][x];
                    best_first = first[i][x];
                }
            }
            types |= best << (2 * i);
        }
        return types;
    };

    report(StageWriting, 0, written_vertices.size() + held_vertices.size());
    std::set<std::pair<u128, u64>> vertex_keys; // (environment, attribute types)
    std::vector<std::pair<u64, u64>> patches;  // key offset, attribute types
    for (const auto& written : written_vertices) {
        const auto& program = graphics[written.program];
        const u64 types = attribute_types(program);
        vertex_keys.emplace(program.environment, types);
        patches.emplace_back(written.key_offset + AttributeTypesOffset, types);
    }
    for (auto& held : held_vertices) {
        const auto& program = graphics[held.program];
        const u64 types = attribute_types(program);
        if (!vertex_keys.emplace(program.environment, types).second) {
            continue;
        }
        const auto key = GraphicsEntryKey(0, vertex_template, program.hash, types);
        Write(graphics_out, u32{1});
        graphics_out.write(reinterpret_cast<const char*>(held.env.data()),
                           static_cast<std::streamsize>(held.env.size()));
        graphics_out.write(reinterpret_cast<const char*>(key.data()),
                           static_cast<std::streamsize>(key.size()));
        held.env = {};
    }
    result.vertex_programs = vertex_keys.size();
    if (warm_tess) {
        graphics_out.flush();
        std::ifstream back(graphics_tmp, std::ios::binary);
        std::set<std::pair<u64, u64>> written_pairs;
        for (const auto& [group, members] : tess_groups) {
            // Groups without a tessellation stage stop here: most variations.
            if (!members.vertex || !members.tess) {
                continue;
            }
            const GraphicsProgram& vertex = graphics[*members.vertex];
            const GraphicsProgram& tess = graphics[*members.tess];
            if (!vertex.built || !tess.built) {
                continue;
            }
            const auto span_it = vertex_env_spans.find(vertex.environment);
            const auto tess_it = tess_envs.find(*members.tess);
            if (span_it == vertex_env_spans.end() || tess_it == tess_envs.end() || !back) {
                continue;
            }
            if (!written_pairs.emplace(vertex.hash, tess.hash).second) {
                continue;
            }
            std::vector<u8> vertex_env(static_cast<size_t>(span_it->second.second));
            back.seekg(static_cast<std::streamoff>(span_it->second.first));
            back.read(reinterpret_cast<char*>(vertex_env.data()),
                      static_cast<std::streamsize>(vertex_env.size()));
            if (!back) {
                back.clear();
                continue;
            }
            GraphicsKey key = *stats.templates[2];
            std::memcpy(key.data() + 1 * 8, &vertex.hash, sizeof(u64));
            std::memcpy(key.data() + 3 * 8, &tess.hash, sizeof(u64));
            std::memset(key.data() + 5 * 8, 0, sizeof(u64));
            const u64 types = attribute_types(vertex);
            std::memcpy(key.data() + AttributeTypesOffset, &types, sizeof(types));
            Write(graphics_out, u32{2});
            graphics_out.write(reinterpret_cast<const char*>(vertex_env.data()),
                               static_cast<std::streamsize>(vertex_env.size()));
            graphics_out.write(reinterpret_cast<const char*>(tess_it->second.data()),
                               static_cast<std::streamsize>(tess_it->second.size()));
            graphics_out.write(reinterpret_cast<const char*>(key.data()),
                               static_cast<std::streamsize>(key.size()));
            ++result.tess_pipelines;
        }
    }
    compute_out.close();
    graphics_out.close();
    if (!compute_out || !graphics_out) {
        return fail("Writing the precache files failed");
    }
    {
        std::fstream patch(graphics_tmp, std::ios::binary | std::ios::in | std::ios::out);
        if (!patch) {
            return fail("Cannot reopen " + Common::FS::PathToUTF8String(graphics_tmp));
        }
        size_t done = 0;
        for (const auto& [offset, types] : patches) {
            patch.seekp(static_cast<std::streamoff>(offset));
            patch.write(reinterpret_cast<const char*>(&types), sizeof(types));
            if (++done % 4096 == 0) {
                report(StageWriting, done, patches.size());
            }
        }
        if (!patch) {
            return fail("Writing the precache files failed");
        }
    }
    // New files: the markers and counts of the old ones no longer apply. Each is removed right
    // before its file is replaced, so a failed replacement leaves the other file's marker intact.
    // A marker is dropped only when its file's contents change (the marker also carries the
    // driver and variant, checked at boot).
    std::error_code ignored;
    DropStaleWarmMarker(compute_path, compute_tmp);
    fs::remove(WarmCountPath(compute_path), ignored);
    fs::rename(compute_tmp, compute_path, ec);
    if (ec) {
        return fail("Cannot replace " + Common::FS::PathToUTF8String(compute_path));
    }
    DropStaleWarmMarker(graphics_path, graphics_tmp);
    fs::remove(WarmCountPath(graphics_path), ignored);
    fs::rename(graphics_tmp, graphics_path, ec);
    if (ec) {
        return fail("Cannot replace " + Common::FS::PathToUTF8String(graphics_path));
    }
    WriteWarmCount(compute_path, result.compute_programs);
    WriteWarmCount(graphics_path,
                   result.vertex_programs + result.fragment_programs + result.tess_pipelines);
    report(StageWriting, patches.size(), patches.size());
    result.ok = true;
    if (warm_skipped) {
        // Every compute key of the precache, for LICHII_COMPUTE_ORIGIN at run time: StageHash,
        // shared memory, workgroup, and 1 when the workgroup was guessed.
        std::ofstream index_out(output_dir / "warm-compute-index.txt", std::ios::trunc);
        for (size_t i = 0; i < compute_index.size(); ++i) {
            u64 hash{};
            std::array<u32, 4> rest{};
            std::memcpy(&hash, compute_index[i].data(), sizeof(hash));
            std::memcpy(rest.data(), compute_index[i].data() + 8, sizeof(rest));
            index_out << fmt::format("{:016x} {} {} {} {} {}\n", hash, rest[0], rest[1], rest[2],
                                     rest[3], compute_index_guessed[i] ? 1 : 0);
        }
        LOG_INFO(Render_Vulkan,
                 "Shader precache skipped programs: {} compute without a workgroup size ({} "
                 "entries guessed), {} with a short control, {} compute and {} graphics not "
                 "converted",
                 skipped_no_workgroup, guessed_compute, skipped_short_control,
                 skipped_compute_conversion, skipped_graphics_conversion);
        std::vector<std::pair<size_t, std::string>> kinds;
        for (const auto& [kind, count] : member_error_kinds) {
            kinds.emplace_back(count, kind);
        }
        std::ranges::sort(kinds, std::greater{});
        for (size_t i = 0; i < std::min<size_t>(kinds.size(), 12); ++i) {
            LOG_INFO(Render_Vulkan, "Shader precache member errors: {} x {}", kinds[i].first,
                     kinds[i].second);
        }
    }
    LOG_INFO(Render_Vulkan,
             "Shader precache build for {:016x}: {} compute, {} vertex, {} fragment, {} "
             "tessellated entries, {} programs skipped, {} archive and {} member errors, {} pack "
             "frames ({} failed, {} too large), {:.1f} s",
             title_id, result.compute_programs, result.vertex_programs, result.fragment_programs,
             result.tess_pipelines, result.skipped, archive_errors, member_errors, frames.frames,
             frames.failed, frames.oversized,
             std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count());
    return result;
}

} // namespace VideoCommon::ShaderWarm
