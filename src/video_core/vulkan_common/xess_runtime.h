// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// lichii-arc: Intel XeSS Super Resolution through Intel's runtime (libxess.dll), loaded at run
// time from the emulator's folder. Only the SDK's MIT headers are compiled in. Opt-in: the
// Ocarina upscale uses it with EDEN_NX2_DLSS_REPLACEMENT=xess or LICHII_OOT_XESS=1, and anything
// that goes wrong leaves FSR 3.1 in charge.

#pragma once

#include <functional>
#include <memory>
#include <set>
#include <string>

#include "video_core/vulkan_common/vulkan.h"

#include "xess/xess_vk.h"

namespace Vulkan::XessRuntime {

struct Api {
    decltype(&xessGetVersion) get_version{};
    decltype(&xessVKGetRequiredInstanceExtensions) required_instance_extensions{};
    decltype(&xessVKGetRequiredDeviceExtensions) required_device_extensions{};
    decltype(&xessVKGetRequiredDeviceFeatures) required_device_features{};
    decltype(&xessVKCreateContext) create_context{};
    decltype(&xessVKInit) init{};
    decltype(&xessVKExecute) execute{};
    decltype(&xessDestroyContext) destroy_context{};
    decltype(&xessSetLoggingCallback) set_logging_callback{};
    decltype(&xessIsOptimalDriver) is_optimal_driver{};
    decltype(&xessSetJitterScale) set_jitter_scale{};
    decltype(&xessSetVelocityScale) set_velocity_scale{};
    decltype(&xessGetIntelXeFXVersion) get_xefx_version{};
    /// lichii-arc: optional (LICHII_XESS_EARLY_INIT): build XeSS's pipelines ahead of xessVKInit.
    decltype(&xessVKBuildPipelines) build_pipelines{};
    decltype(&xessGetPipelineBuildStatus) get_pipeline_build_status{};
    std::string version;
};

/// Whether the user asked for XeSS (EDEN_NX2_DLSS_REPLACEMENT=xess or LICHII_OOT_XESS=1). Always
/// false outside Windows.
[[nodiscard]] bool Wanted();

/// libxess.dll's entry points, or null when it isn't wanted, isn't there or lacks an export (the
/// log says which). Loaded once per session.
[[nodiscard]] std::shared_ptr<const Api> Acquire();

/// Called while the device is being created: adds XeSS's device extensions to `extensions` and
/// its features to the pNext chain `next`, only when the device supports every extension.
/// Returns whether it did (XeSS can then be created on that device).
bool AddDeviceRequirements(VkInstance instance, VkPhysicalDevice physical,
                           const std::function<bool(const std::string&)>& supported,
                           std::set<std::string, std::less<>>& extensions, const void*& next);

/// Undoes AddDeviceRequirements' effect on the record: the device was created without them.
void DropDeviceRequirements();

/// Whether the logical device was created with XeSS's requirements.
[[nodiscard]] bool DeviceReady();

} // namespace Vulkan::XessRuntime
