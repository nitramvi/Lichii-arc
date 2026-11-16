// SPDX-FileCopyrightText: Copyright 2026 lichii-arc
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Common::GameFileTrace {

/// lichii-arc: which game file the guest reads. Off unless LICHII_ZONE_PREWARM=1 or
/// LICHII_SHADER_PROVENANCE=1: then the file system service reports the RomFS file under each
/// storage read (Note), and video_core listens (zone prewarm) and asks for the last one
/// (shader provenance).
[[nodiscard]] bool Enabled();

/// Called on the guest's file system thread when a read lands in a RomFS file other than the
/// previous one ('/' separated path inside the RomFS).
void Note(std::string_view path);

/// The last file Note reported, empty before the first.
[[nodiscard]] std::string LastFile();

/// The last RecentCount distinct files Note reported, the newest first.
constexpr size_t RecentCount = 4;
[[nodiscard]] std::vector<std::string> RecentFiles();

/// One listener (video_core's zone prewarm); replaces the previous one. Called outside the
/// trace's lock, on the reporting thread, for every Note. Pass an empty function to remove it.
void SetListener(std::function<void(const std::string&)> listener);

} // namespace Common::GameFileTrace
