// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <mutex>
#include <string_view>

#include <fmt/format.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/vulkan_common/xess_runtime.h"

namespace Vulkan::XessRuntime {
namespace {
bool device_ready = false;

#ifdef _WIN32
std::shared_ptr<const Api> Load() {
    HMODULE module = LoadLibraryW(L"libxess.dll");
    if (module == nullptr) {
        LOG_WARNING(Render_Vulkan, "XeSS: libxess.dll not found next to the emulator; using FSR 3.1");
        return nullptr;
    }
    auto api = std::make_shared<Api>();
    bool complete = true;
    const auto get = [&](auto& pointer, const char* name) {
        pointer = reinterpret_cast<std::remove_reference_t<decltype(pointer)>>(
            reinterpret_cast<void*>(GetProcAddress(module, name)));
        if (pointer == nullptr) {
            LOG_WARNING(Render_Vulkan, "XeSS: libxess.dll lacks {}", name);
            complete = false;
        }
    };
    get(api->get_version, "xessGetVersion");
    get(api->required_instance_extensions, "xessVKGetRequiredInstanceExtensions");
    get(api->required_device_extensions, "xessVKGetRequiredDeviceExtensions");
    get(api->required_device_features, "xessVKGetRequiredDeviceFeatures");
    get(api->create_context, "xessVKCreateContext");
    get(api->init, "xessVKInit");
    get(api->execute, "xessVKExecute");
    get(api->destroy_context, "xessDestroyContext");
    get(api->set_logging_callback, "xessSetLoggingCallback");
    get(api->is_optimal_driver, "xessIsOptimalDriver");
    get(api->set_jitter_scale, "xessSetJitterScale");
    get(api->set_velocity_scale, "xessSetVelocityScale");
    get(api->get_xefx_version, "xessGetIntelXeFXVersion");
    // Optional: without them the pipelines build inside xessVKInit as before.
    api->build_pipelines = reinterpret_cast<decltype(api->build_pipelines)>(
        reinterpret_cast<void*>(GetProcAddress(module, "xessVKBuildPipelines")));
    api->get_pipeline_build_status = reinterpret_cast<decltype(api->get_pipeline_build_status)>(
        reinterpret_cast<void*>(GetProcAddress(module, "xessGetPipelineBuildStatus")));
    if (!complete) {
        LOG_WARNING(Render_Vulkan, "XeSS: libxess.dll is incomplete; using FSR 3.1");
        return nullptr;
    }
    xess_version_t version{};
    if (api->get_version(&version) == XESS_RESULT_SUCCESS) {
        api->version = fmt::format("{}.{}.{}", version.major, version.minor, version.patch);
    }
    LOG_INFO(Render_Vulkan, "XeSS: loaded libxess.dll {}", api->version);
    return api;
}
#endif
} // Anonymous namespace

bool Wanted() {
#ifdef _WIN32
    // Read each time: the OoT preset in the settings sets it when a game starts.
    const char* replacement = std::getenv("EDEN_NX2_DLSS_REPLACEMENT");
    const char* flag = std::getenv("LICHII_OOT_XESS");
    return (replacement && std::string_view{replacement} == "xess") || (flag && flag[0] == '1');
#else
    return false;
#endif
}

std::shared_ptr<const Api> Acquire() {
#ifdef _WIN32
    if (!Wanted()) {
        return nullptr;
    }
    static std::once_flag once;
    static std::shared_ptr<const Api> api;
    std::call_once(once, [] { api = Load(); });
    return api;
#else
    return nullptr;
#endif
}

bool AddDeviceRequirements(VkInstance instance, VkPhysicalDevice physical,
                           const std::function<bool(const std::string&)>& supported,
                           std::set<std::string, std::less<>>& extensions, const void*& next) {
    device_ready = false;
    const auto api = Acquire();
    if (!api) {
        return false;
    }
    u32 instance_count = 0;
    const char* const* instance_names = nullptr;
    u32 min_api = 0;
    if (api->required_instance_extensions(&instance_count, &instance_names, &min_api) ==
        XESS_RESULT_SUCCESS) {
        std::string names;
        for (u32 i = 0; i < instance_count; ++i) {
            names += fmt::format("{}{}", names.empty() ? "" : ", ", instance_names[i]);
        }
        LOG_INFO(Render_Vulkan, "XeSS: instance needs Vulkan {}.{} and {}",
                 VK_API_VERSION_MAJOR(min_api), VK_API_VERSION_MINOR(min_api),
                 names.empty() ? "no extensions" : names);
    }
    u32 count = 0;
    const char* const* names = nullptr;
    if (api->required_device_extensions(instance, physical, &count, &names) !=
        XESS_RESULT_SUCCESS) {
        LOG_WARNING(Render_Vulkan, "XeSS: can't list its device extensions; using FSR 3.1");
        return false;
    }
    std::string list;
    std::string missing;
    for (u32 i = 0; i < count; ++i) {
        const std::string name{names[i]};
        list += (list.empty() ? "" : ", ") + name;
        if (!supported(name)) {
            missing += (missing.empty() ? "" : ", ") + name;
        }
    }
    if (!missing.empty()) {
        LOG_WARNING(Render_Vulkan, "XeSS: the device lacks {}; using FSR 3.1", missing);
        return false;
    }
    // XeSS patches the features it needs into the chain and may prepend its own structures.
    void* chain = const_cast<void*>(next);
    if (api->required_device_features(instance, physical, &chain) != XESS_RESULT_SUCCESS) {
        LOG_WARNING(Render_Vulkan, "XeSS: can't add its device features; using FSR 3.1");
        return false;
    }
    for (u32 i = 0; i < count; ++i) {
        extensions.emplace(names[i]);
    }
    next = chain;
    device_ready = true;
    LOG_INFO(Render_Vulkan, "XeSS: device extensions enabled ({})",
             list.empty() ? "none" : list);
    return true;
}

void DropDeviceRequirements() {
    device_ready = false;
}

bool DeviceReady() {
    return device_ready;
}

} // namespace Vulkan::XessRuntime
