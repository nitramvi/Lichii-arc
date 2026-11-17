// SPDX-FileCopyrightText: Copyright 2026 lichii-arc
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

namespace VideoCommon::ShaderProvenance {

/// LICHII_SHADER_PROVENANCE=1: every pipeline whose driver build keeps the game waiting past
/// LICHII_SHADER_PROVENANCE_MS (default 100) is logged (LICHII_PROVENANCE) and appended to
/// lichii-provenance.txt beside the title's shader cache, with the game files the guest read
/// last before it (Common::GameFileTrace). The precache found it in none of its sources.
[[nodiscard]] bool Enabled();

/// LICHII_ZONE_PREWARM=1: when the guest reads a game file that earlier sessions' provenance
/// ties to slow pipelines, those pipelines are queued for a background build (PipelineCache).
[[nodiscard]] bool ZonePrewarm();

/// Reads the title's provenance file (earlier sessions) and opens it for this session's lines.
void Open(const std::filesystem::path& title_dir);

enum class Kind { Graphics, Compute };

/// A pipeline whose build kept the game waiting `ms`. `hash` is the pipeline key's hash for
/// graphics, the shader's for compute; `detail` is free text (stages, address).
void NoteSlowBuild(Kind kind, u64 hash, double ms, std::string_view detail = {});

/// Graphics pipeline key hashes earlier sessions tied to `game_file`.
[[nodiscard]] std::vector<u64> GraphicsFor(const std::string& game_file);

} // namespace VideoCommon::ShaderProvenance
