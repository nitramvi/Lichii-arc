// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "common/common_types.h"
#include "common/logging/log.h"
#include "video_core/spirv_inline_control.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

namespace {
// lichii-arc (from ibsorn/Lichii, zortos293/Lichii#579): functions a shader calls from several
// places (a native program's global memory functions, hundreds of instructions each, called from
// dozens of sites) are built as calls instead of being inlined everywhere. On NVIDIA that took
// single shaders from 2-13 s to a fraction; Intel Arc spent 1-22 s per Ocarina of Time pipeline.
// LICHII_NOINLINE=0 builds every module exactly as emitted.
bool NoInlineEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("LICHII_NOINLINE");
        const bool on = value == nullptr || value[0] != '0';
        LOG_INFO(Render_Vulkan, "Shared shader functions {}", on ? "built as calls (DontInline)"
                                                                : "inlined as emitted");
        return on;
    }();
    return enabled;
}
} // Anonymous namespace

vk::ShaderModule BuildShader(const Device& device, std::span<const u32> code) {
    if (NoInlineEnabled()) {
        std::vector<u32> words(code.begin(), code.end());
        if (const std::size_t marked = VideoCommon::MarkSharedFunctionsDontInline(words);
            marked != 0) {
            static std::atomic<u32> logged{};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 16) {
                LOG_INFO(Render_Vulkan, "Shader module: {} shared functions built as calls",
                         marked);
            }
            return device.GetLogical().CreateShaderModule({
                .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .codeSize = static_cast<u32>(words.size() * sizeof(u32)),
                .pCode = words.data(),
            });
        }
    }
    return device.GetLogical().CreateShaderModule({
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = static_cast<u32>(code.size_bytes()),
        .pCode = code.data(),
    });
}

} // namespace Vulkan
