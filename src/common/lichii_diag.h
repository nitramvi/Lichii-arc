// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// lichii-arc: extended diagnostics for testing on handhelds, all behind LICHII_DIAG=1.
// - Marks: the frontend calls Mark() when the player presses F9; the log gets "LICHII_MARK n"
//   (and the frame number at the next present) and the frontend saves a screenshot.
// - Telemetry: every 2 s, CPU frequency of the performance and efficiency cores, the limit the
//   firmware applies to them (throttling), and the GPU's 3D engine utilization (Windows only).
// - GPU time: sampled GPU busy time of frames, summarized with the 10 s stutter summary.
// - Hitches: the video core logs "LICHII_HITCH" for every frame over 100 ms with its main cause in
//   plain words and the last game file opened.

#pragma once

#include <string>
#include <string_view>

#include "common/common_types.h"

namespace Common::LichiiDiag {

/// LICHII_DIAG=1.
[[nodiscard]] bool Enabled();

/// Starts the telemetry thread (idempotent; nothing without LICHII_DIAG or outside Windows).
void StartTelemetry();

/// A manual mark from the player. Returns its number (0 when diagnostics are off).
u32 Mark();

/// The mark waiting for the next present's frame number (0: none), cleared by the call.
[[nodiscard]] u32 TakePendingMark();

/// The last file of the game the guest opened, as the file system saw it.
void NoteFileOpened(std::string_view path);
[[nodiscard]] std::string LastFileOpened();
/// Seconds since the last file open (a large value when none was).
[[nodiscard]] double SecondsSinceFileOpened();

/// Something rare and slow that the hitch line should name when it lands in a long frame
/// (for example "xess_init").
void NoteEvent(std::string_view event);
/// The events noted since the last call, comma separated.
[[nodiscard]] std::string TakeEvents();

/// One sampled frame's GPU busy time.
void AddGpuFrameSample(double busy_ms);
/// "gpu_ms=avg:x,max:y,samples:n" for the samples since the last call (or "gpu_ms=none").
[[nodiscard]] std::string TakeGpuSummary();

} // namespace Common::LichiiDiag
