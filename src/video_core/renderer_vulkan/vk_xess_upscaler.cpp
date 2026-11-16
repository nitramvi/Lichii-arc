// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Intel XeSS-SR takes plain Vulkan image views: the inputs in SHADER_READ_ONLY_OPTIMAL, the output
// in GENERAL, and records into our command buffer.

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <boost/container/static_vector.hpp>
#include <fmt/format.h>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/lichii_diag.h"
#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_xess_upscaler.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/xess_runtime.h"

namespace Vulkan {
namespace {
constexpr VkFormat OUTPUT_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkImageUsageFlags OUTPUT_USAGE = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_DST_BIT;
constexpr VkImageSubresourceRange COLOR_RANGE{
    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1,
};

VkImageMemoryBarrier Barrier(VkImage image, VkImageSubresourceRange range, VkAccessFlags src_access,
                             VkAccessFlags dst_access, VkImageLayout old_layout,
                             VkImageLayout new_layout) {
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = range,
    };
}

VkImageMemoryBarrier Barrier(const XessUpscaler::Image& image, VkAccessFlags src_access,
                             VkAccessFlags dst_access, VkImageLayout old_layout,
                             VkImageLayout new_layout) {
    VkImageSubresourceRange range = image.range;
    range.aspectMask = image.barrier_aspect;
    return Barrier(image.image, range, src_access, dst_access, old_layout, new_layout);
}

xess_vk_image_view_info View(const XessUpscaler::Image& image) {
    return xess_vk_image_view_info{
        .imageView = image.view,
        .image = image.image,
        .subresourceRange = image.range,
        .format = image.format,
        .width = image.size.width,
        .height = image.size.height,
    };
}

void Message(const char* message, xess_logging_level_t level) {
    if (level >= XESS_LOGGING_LEVEL_ERROR) {
        LOG_ERROR(Render_Vulkan, "XeSS: {}", message ? message : "");
    } else if (level >= XESS_LOGGING_LEVEL_WARNING) {
        LOG_WARNING(Render_Vulkan, "XeSS: {}", message ? message : "");
    }
}

/// The XeSS quality whose scale is nearest the render-to-output ratio (Ocarina's render scales are
/// 2x and 4/3x in handheld).
xess_quality_settings_t QualityFor(VkExtent2D render, VkExtent2D upscale) {
    const f32 ratio = static_cast<f32>(upscale.width) / static_cast<f32>(render.width);
    struct Entry {
        f32 scale;
        xess_quality_settings_t quality;
    };
    constexpr Entry entries[]{
        {3.0f, XESS_QUALITY_SETTING_ULTRA_PERFORMANCE}, {2.0f, XESS_QUALITY_SETTING_PERFORMANCE},
        {1.7f, XESS_QUALITY_SETTING_BALANCED},          {1.5f, XESS_QUALITY_SETTING_QUALITY},
        {1.3f, XESS_QUALITY_SETTING_ULTRA_QUALITY},     {1.0f, XESS_QUALITY_SETTING_AA},
    };
    const Entry* best = &entries[0];
    for (const Entry& entry : entries) {
        if (std::abs(entry.scale - ratio) < std::abs(best->scale - ratio)) {
            best = &entry;
        }
    }
    return best->quality;
}

/// "x,y" from an environment variable (debugging the axis conventions), or {1, 1}.
std::array<f32, 2> ScaleFromEnv(const char* name) {
    std::array<f32, 2> scale{1.0f, 1.0f};
    if (const char* value = std::getenv(name)) {
        char* end = nullptr;
        scale[0] = std::strtof(value, &end);
        if (end && *end == ',') {
            scale[1] = std::strtof(end + 1, nullptr);
        }
    }
    return scale;
}
/// lichii-arc: where LICHII_XESS_EARLY_INIT keeps XeSS's pipeline cache (one per installation;
/// the driver ignores data from another device or driver version).
std::filesystem::path PipelineCachePath() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir) / "xess_vk_pipelines.bin";
}
} // Anonymous namespace

bool XessUpscaler::EarlyInitWanted() {
    static const bool wanted = [] {
        const char* value = std::getenv("LICHII_XESS_EARLY_INIT");
        return value && value[0] == '1';
    }();
    return wanted;
}

u32 XessUpscaler::InitFlags() const {
    // Tonemapped colour, standard depth, render-size motion vectors in pixels.
    u32 flags = XESS_INIT_FLAG_LDR_INPUT_COLOR;
    // lichii-arc: the automatic responsive mask (vk_ocarina_camera_motion.cpp) comes every frame.
    if (ResponsiveMaskWanted()) {
        flags |= XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK;
    }
    return flags;
}

void XessUpscaler::StartPipelineBuild() {
    if (!EarlyInitWanted() || !api->build_pipelines || !api->get_pipeline_build_status) {
        return;
    }
    std::vector<u8> data;
    const auto path = PipelineCachePath();
    if (Common::FS::Exists(path)) {
        Common::FS::IOFile file{path, Common::FS::FileAccessMode::Read,
                                Common::FS::FileType::BinaryFile};
        if (file.IsOpen()) {
            data.resize(file.GetSize());
            if (file.ReadSpan(std::span<u8>{data}) != data.size()) {
                data.clear();
            }
        }
    }
    try {
        pipeline_cache = device.GetLogical().CreatePipelineCache(VkPipelineCacheCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .initialDataSize = data.size(),
            .pInitialData = data.empty() ? nullptr : data.data(),
        });
    } catch (const vk::Exception&) {
        // Data the driver rejects: start empty.
        pipeline_cache = device.GetLogical().CreatePipelineCache(VkPipelineCacheCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .initialDataSize = 0,
            .pInitialData = nullptr,
        });
    }
    build_begin = std::chrono::steady_clock::now();
    const xess_result_t result = api->build_pipelines(static_cast<xess_context_handle_t>(context),
                                                      *pipeline_cache, false, InitFlags());
    if (result != XESS_RESULT_SUCCESS) {
        LOG_WARNING(Render_Vulkan,
                    "XeSS: xessVKBuildPipelines failed ({}); building inside xessVKInit",
                    static_cast<int>(result));
        return;
    }
    building_pipelines = true;
    LOG_INFO(Render_Vulkan, "XeSS: building its pipelines in the background ({} KiB cached)",
             data.size() / 1024);
}

bool XessUpscaler::PipelinesReady() {
    if (!building_pipelines) {
        return true;
    }
    const xess_result_t status =
        api->get_pipeline_build_status(static_cast<xess_context_handle_t>(context));
    if (status == XESS_RESULT_ERROR_OPERATION_IN_PROGRESS) {
        if (!logged_waiting) {
            logged_waiting = true;
            LOG_INFO(Render_Vulkan, "XeSS: pipelines still building; FSR 3.1 draws until then");
        }
        return false;
    }
    building_pipelines = false;
    LOG_INFO(Render_Vulkan, "XeSS: pipelines built in {:.0f} ms (status {})",
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                       build_begin)
                 .count(),
             static_cast<int>(status));
    SavePipelineCache();
    return true;
}

void XessUpscaler::SavePipelineCache() {
    if (!pipeline_cache) {
        return;
    }
    size_t size = 0;
    if (pipeline_cache.Read(&size, nullptr) != VK_SUCCESS || size == 0) {
        return;
    }
    std::vector<u8> data(size);
    if (pipeline_cache.Read(&size, data.data()) != VK_SUCCESS) {
        return;
    }
    data.resize(size);
    const auto path = PipelineCachePath();
    void(Common::FS::CreateParentDirs(path));
    Common::FS::IOFile file{path, Common::FS::FileAccessMode::Write,
                            Common::FS::FileType::BinaryFile};
    if (file.IsOpen() && file.WriteSpan(std::span<const u8>{data}) == data.size()) {
        LOG_INFO(Render_Vulkan, "XeSS: pipeline cache saved ({} KiB)", data.size() / 1024);
    }
}

bool XessUpscaler::ResponsiveMaskWanted() {
    static const bool wanted = [] {
        const char* value = std::getenv("LICHII_OOT_AUTO_MASK");
        return value && value[0] == '1';
    }();
    return wanted;
}

bool XessUpscaler::IsWanted() {
    return XessRuntime::Wanted() && XessRuntime::DeviceReady();
}

XessUpscaler::XessUpscaler(const Device& device_, Scheduler& scheduler_,
                           MemoryAllocator& memory_allocator_)
    : device{device_}, scheduler{scheduler_}, memory_allocator{memory_allocator_},
      api{XessRuntime::Acquire()} {
    if (!api || !XessRuntime::DeviceReady()) {
        throw vk::Exception(VK_ERROR_FEATURE_NOT_PRESENT);
    }
    xess_context_handle_t handle{};
    const xess_result_t result =
        api->create_context(device.GetInstance(), static_cast<VkPhysicalDevice>(device.GetPhysical()),
                            *device.GetLogical(), &handle);
    if (result != XESS_RESULT_SUCCESS) {
        LOG_ERROR(Render_Vulkan, "XeSS: xessVKCreateContext failed ({}); using FSR 3.1",
                  static_cast<int>(result));
        throw vk::Exception(VK_ERROR_FEATURE_NOT_PRESENT);
    }
    context = handle;
    api->set_logging_callback(handle, XESS_LOGGING_LEVEL_WARNING, Message);
    const bool optimal = api->is_optimal_driver(handle) == XESS_RESULT_SUCCESS;
    xess_version_t xefx{};
    const bool xmx = api->get_xefx_version(handle, &xefx) == XESS_RESULT_SUCCESS;
    LOG_INFO(Render_Vulkan, "XeSS {}: context on {} ({}), {}{}", api->version,
             device.GetModelName(), device.GetDriverName(),
             xmx ? fmt::format("XMX path (XeFX {}.{}.{})", xefx.major, xefx.minor, xefx.patch)
                 : std::string{"DP4a path"},
             optimal ? "" : ", the driver is older than XeSS recommends");
    StartPipelineBuild();
}

XessUpscaler::~XessUpscaler() {
    if (context != nullptr) {
        scheduler.Finish(); // recorded evaluations may still use it
        if (!building_pipelines) {
            SavePipelineCache();
        }
        api->destroy_context(static_cast<xess_context_handle_t>(context));
    }
}

bool XessUpscaler::Init(VkExtent2D render_size, VkExtent2D upscale_size) {
    scheduler.Finish(); // XeSS must not be re-initialized while its work is pending
    const xess_quality_settings_t quality = QualityFor(render_size, upscale_size);
    xess_vk_init_params_t params{};
    params.outputResolution = {upscale_size.width, upscale_size.height};
    params.qualitySetting = quality;
    params.initFlags = InitFlags();
    params.creationNodeMask = 1;
    params.visibleNodeMask = 1;
    params.tempBufferHeap = VK_NULL_HANDLE;
    params.tempTextureHeap = VK_NULL_HANDLE;
    // The same cache xessVKBuildPipelines got (XeSS requires it), or none.
    params.pipelineCache = pipeline_cache ? *pipeline_cache : VK_NULL_HANDLE;
    const auto handle = static_cast<xess_context_handle_t>(context);
    Common::LichiiDiag::NoteEvent("xess_init");
    const xess_result_t result = api->init(handle, &params);
    if (result != XESS_RESULT_SUCCESS) {
        LOG_ERROR(Render_Vulkan, "XeSS: xessVKInit {}x{} -> {}x{} failed ({}); using FSR 3.1",
                  render_size.width, render_size.height, upscale_size.width, upscale_size.height,
                  static_cast<int>(result));
        return false;
    }
    const auto jitter_scale = ScaleFromEnv("LICHII_XESS_JITTER_SCALE");
    const auto velocity_scale = ScaleFromEnv("LICHII_XESS_VELOCITY_SCALE");
    api->set_jitter_scale(handle, jitter_scale[0], jitter_scale[1]);
    api->set_velocity_scale(handle, velocity_scale[0], velocity_scale[1]);
    render_extent = render_size;
    upscale_extent = upscale_size;
    LOG_INFO(Render_Vulkan,
             "XeSS: initialized {}x{} -> {}x{} (quality {}, jitter scale {},{}, velocity scale {},{})",
             render_size.width, render_size.height, upscale_size.width, upscale_size.height,
             static_cast<int>(quality), jitter_scale[0], jitter_scale[1], velocity_scale[0],
             velocity_scale[1]);
    return true;
}

void XessUpscaler::CreateOutput(VkExtent2D upscale_size) {
    if (output_image) {
        scheduler.Finish(); // the old output may still be in use
    }
    output_image = memory_allocator.CreateImage(VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = OUTPUT_FORMAT,
        .extent = {upscale_size.width, upscale_size.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = OUTPUT_USAGE,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
    output_view = device.GetLogical().CreateImageView(VkImageViewCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = *output_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = OUTPUT_FORMAT,
        .components{},
        .subresourceRange = COLOR_RANGE,
    });
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([image = *output_image](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                               Barrier(image, COLOR_RANGE, 0,
                                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                                       VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL));
    });
}

bool XessUpscaler::Upscale(const Inputs& in) {
    if (failed.load()) {
        return false;
    }
    const bool resized = render_extent.width != in.render_size.width ||
                         render_extent.height != in.render_size.height ||
                         upscale_extent.width != in.upscale_size.width ||
                         upscale_extent.height != in.upscale_size.height;
    if (resized && !PipelinesReady()) {
        // lichii-arc: XeSS's pipelines are still building in the background (FSR 3.1 draws).
        return false;
    }
    if (resized) {
        if (!Init(in.render_size, in.upscale_size)) {
            failed = true;
            return false;
        }
        CreateOutput(in.upscale_size);
    }

    // Initialized with the responsive-mask flag, XeSS needs the mask on every evaluation: a frame
    // without one goes to FSR 3.1 (which then resets its history) instead.
    if (ResponsiveMaskWanted() && !in.responsive) {
        return false;
    }
    xess_vk_execute_params_t params{};
    params.colorTexture = View(in.color);
    params.velocityTexture = View(in.motion_vectors);
    params.depthTexture = View(in.depth);
    if (in.responsive) {
        params.responsivePixelMaskTexture = View(*in.responsive);
    }
    params.outputTexture = xess_vk_image_view_info{
        .imageView = *output_view,
        .image = *output_image,
        .subresourceRange = COLOR_RANGE,
        .format = OUTPUT_FORMAT,
        .width = in.upscale_size.width,
        .height = in.upscale_size.height,
    };
    // XeSS takes jitter in [-0.5, 0.5]; a [0, 1) sequence is recentred (a constant half-pixel
    // shift of the whole image, the same every frame).
    params.jitterOffsetX = in.jitter[0] - in.jitter_center;
    params.jitterOffsetY = in.jitter[1] - in.jitter_center;
    params.exposureScale = 1.0f;
    params.resetHistory = in.reset || resized ? 1 : 0;
    params.inputWidth = in.render_size.width;
    params.inputHeight = in.render_size.height;

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, in, params, output = *output_image](vk::CommandBuffer cmdbuf) {
        constexpr VkAccessFlags WRITES =
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        // The inputs stay in GENERAL otherwise; XeSS reads them as sampled images.
        boost::container::static_vector<VkImageMemoryBarrier, 5> to_read{
            Barrier(in.color, WRITES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(in.depth, WRITES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(in.motion_vectors, WRITES, VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(output, COLOR_RANGE, VK_ACCESS_SHADER_READ_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL),
        };
        if (in.responsive) {
            to_read.push_back(Barrier(*in.responsive, WRITES, VK_ACCESS_SHADER_READ_BIT,
                                      VK_IMAGE_LAYOUT_GENERAL,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
        }
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, to_read);

        const xess_result_t result =
            api->execute(static_cast<xess_context_handle_t>(context), *cmdbuf, &params);
        if (result != XESS_RESULT_SUCCESS && !failed.exchange(true)) {
            LOG_ERROR(Render_Vulkan, "XeSS: xessVKExecute failed ({}); using FSR 3.1",
                      static_cast<int>(result));
        }

        boost::container::static_vector<VkImageMemoryBarrier, 5> back{
            Barrier(in.color, VK_ACCESS_SHADER_READ_BIT, 0,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(in.depth, VK_ACCESS_SHADER_READ_BIT, 0,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(in.motion_vectors, VK_ACCESS_SHADER_READ_BIT, 0,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(output, COLOR_RANGE, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL),
        };
        if (in.responsive) {
            back.push_back(Barrier(*in.responsive, VK_ACCESS_SHADER_READ_BIT, 0,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                   VK_IMAGE_LAYOUT_GENERAL));
        }
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, {}, {}, back);
    });
    return true;
}

} // namespace Vulkan
