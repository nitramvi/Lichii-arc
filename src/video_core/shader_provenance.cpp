// SPDX-FileCopyrightText: Copyright 2026 lichii-arc
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>

#include "common/game_file_trace.h"
#include "common/logging.h"
#include "video_core/shader_provenance.h"

namespace VideoCommon::ShaderProvenance {
namespace {

constexpr std::string_view FileName = "lichii-provenance.txt";
constexpr u64 MaxFileBytes = 8ULL << 20;

bool EnvOn(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] == '1';
}

double ThresholdMs() {
    static const double threshold = [] {
        const char* value = std::getenv("LICHII_SHADER_PROVENANCE_MS");
        if (value == nullptr) {
            return 100.0;
        }
        char* end{};
        const double parsed = std::strtod(value, &end);
        return end != value && parsed >= 0.0 ? parsed : 100.0;
    }();
    return threshold;
}

std::mutex mutex;
std::filesystem::path file_path;
u64 file_bytes{};
std::unordered_map<std::string, std::unordered_set<u64>> graphics_by_file;
std::unordered_set<u64> noted_this_session;

void Load(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        return;
    }
    std::string line;
    size_t lines = 0;
    while (std::getline(in, line)) {
        // kind \t hash \t ms \t file1|file2|... \t detail
        std::istringstream fields(line);
        std::string kind, hash, ms, files;
        if (!std::getline(fields, kind, '\t') || !std::getline(fields, hash, '\t') ||
            !std::getline(fields, ms, '\t') || !std::getline(fields, files, '\t')) {
            continue;
        }
        if (kind != "G" || files.empty()) {
            continue;
        }
        char* end{};
        const u64 value = std::strtoull(hash.c_str(), &end, 16);
        if (end == hash.c_str()) {
            continue;
        }
        std::istringstream names(files);
        std::string name;
        while (std::getline(names, name, '|')) {
            if (!name.empty()) {
                graphics_by_file[name].insert(value);
            }
        }
        ++lines;
    }
    LOG_INFO(Render_Vulkan,
             "lichii-arc shader provenance: {} graphics lines from earlier sessions, tied to {} "
             "game files",
             lines, graphics_by_file.size());
}

} // Anonymous namespace

bool Enabled() {
    static const bool enabled = EnvOn("LICHII_SHADER_PROVENANCE");
    return enabled;
}

bool ZonePrewarm() {
    static const bool enabled = EnvOn("LICHII_ZONE_PREWARM");
    return enabled;
}

void Open(const std::filesystem::path& title_dir) {
    if (!Enabled() && !ZonePrewarm()) {
        return;
    }
    std::scoped_lock lock{mutex};
    file_path = title_dir / FileName;
    graphics_by_file.clear();
    noted_this_session.clear();
    std::error_code ec;
    file_bytes = std::filesystem::exists(file_path, ec)
                     ? static_cast<u64>(std::filesystem::file_size(file_path, ec))
                     : 0;
    if (ec) {
        file_bytes = 0;
    }
    Load(file_path);
}

void NoteSlowBuild(Kind kind, u64 hash, double ms, std::string_view detail) {
    if (!Enabled() || ms < ThresholdMs()) {
        return;
    }
    std::string files;
    for (const auto& file : Common::GameFileTrace::RecentFiles()) {
        if (!files.empty()) {
            files += '|';
        }
        files += file;
    }
    const char tag = kind == Kind::Graphics ? 'G' : 'C';
    LOG_INFO(Render_Vulkan, "LICHII_PROVENANCE {} {:016x} {:.1f} ms after [{}] {}", tag, hash, ms,
             files, detail);
    std::scoped_lock lock{mutex};
    if (file_path.empty() || file_bytes >= MaxFileBytes ||
        !noted_this_session.insert(hash ^ (kind == Kind::Compute ? 1ULL << 63 : 0)).second) {
        return;
    }
    const std::string line = fmt::format("{}\t{:016x}\t{:.1f}\t{}\t{}\n", tag, hash, ms, files,
                                         detail);
    std::ofstream out(file_path, std::ios::app);
    if (out) {
        out << line;
        file_bytes += line.size();
    }
}

std::vector<u64> GraphicsFor(const std::string& game_file) {
    std::scoped_lock lock{mutex};
    const auto it = graphics_by_file.find(game_file);
    if (it == graphics_by_file.end()) {
        return {};
    }
    return {it->second.begin(), it->second.end()};
}

} // namespace VideoCommon::ShaderProvenance
