// SPDX-FileCopyrightText: Copyright 2026 lichii-arc
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <set>

#include "common/logging.h"
#include <memory>
#include <mutex>

#include "common/game_file_trace.h"

namespace Common::GameFileTrace {
namespace {

bool EnvOn(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] == '1';
}

std::mutex mutex;
std::string last_file;
std::vector<std::string> recent;
std::shared_ptr<const std::function<void(const std::string&)>> listener;

} // Anonymous namespace

bool Enabled() {
    static const bool enabled = EnvOn("LICHII_ZONE_PREWARM") || EnvOn("LICHII_SHADER_PROVENANCE") ||
                                EnvOn("LICHII_FILE_LOG");
    return enabled;
}

// lichii-arc: LICHII_FILE_LOG=1 logs the first time each game file whose path names a voice,
// sound or language is read (up to 400), to see which voice language the game picked.
static void LogLanguageFile(const std::string& path) {
    static const bool on = EnvOn("LICHII_FILE_LOG");
    if (!on) {
        return;
    }
    std::string lower = path;
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    });
    static constexpr const char* Keys[] = {"voice", "vo_", "/vo/", "lang",  "sound", "audio", "message",
                                           "msg",   "usen", "uses", "eues", "jpja",  "usfr"};
    bool match = false;
    for (const char* key : Keys) {
        if (lower.find(key) != std::string::npos) {
            match = true;
            break;
        }
    }
    if (!match) {
        return;
    }
    static std::mutex log_mutex;
    static std::set<std::string> seen;
    std::scoped_lock lock{log_mutex};
    if (seen.size() >= 400 || !seen.insert(path).second) {
        return;
    }
    LOG_INFO(Service_FS, "LICHII_FILE_LOG {}", path);
}

void Note(std::string_view path) {
    if (!Enabled()) {
        return;
    }
    std::shared_ptr<const std::function<void(const std::string&)>> current;
    std::string copy{path};
    LogLanguageFile(copy);
    {
        std::scoped_lock lock{mutex};
        last_file = copy;
        if (const auto it = std::find(recent.begin(), recent.end(), copy); it != recent.end()) {
            recent.erase(it);
        }
        recent.insert(recent.begin(), copy);
        if (recent.size() > RecentCount) {
            recent.pop_back();
        }
        current = listener;
    }
    if (current && *current) {
        (*current)(copy);
    }
}

std::string LastFile() {
    std::scoped_lock lock{mutex};
    return last_file;
}

std::vector<std::string> RecentFiles() {
    std::scoped_lock lock{mutex};
    return recent;
}

void SetListener(std::function<void(const std::string&)> new_listener) {
    auto shared = new_listener
                      ? std::make_shared<const std::function<void(const std::string&)>>(
                            std::move(new_listener))
                      : nullptr;
    std::scoped_lock lock{mutex};
    listener = std::move(shared);
}

} // namespace Common::GameFileTrace
