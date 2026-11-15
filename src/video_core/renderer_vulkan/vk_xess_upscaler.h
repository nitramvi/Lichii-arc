// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// lichii-arc: Intel XeSS Super Resolution through Intel's runtime (vulkan_common/xess_runtime.h),
// with the inputs of the Ocarina FSR 3.1 path, which takes over when XeSS is off, missing or fails.

#pragma once

#include <array>
#include <atomic>
#include <memory>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class MemoryAllocator;
class Scheduler;

namespace XessRuntime {
struct Api;
}

class XessUpscaler {
public:
    struct Image {
        VkImageView view;
        VkImage image;
        VkFormat format;
        VkImageSubresourceRange range;     ///< The used level and layer, with the view's aspect.
        VkImageAspectFlags barrier_aspect; ///< Every aspect of the image's format.
        VkExtent2D size;                   ///< The image's size.
    };

    struct Inputs {
        Image color;          ///< Tonemapped (LDR) colour at the render size.
        Image depth;          ///< Standard (not reversed) device depth.
        Image motion_vectors; ///< Motion to the previous frame, in render pixels.
        VkExtent2D render_size;
        VkExtent2D upscale_size;
        bool reset;
        std::array<f32, 2> jitter{}; ///< Offset the scene was drawn with, in render pixels.
        f32 jitter_center = 0.0f;    ///< Mean of the jitter sequence (0.5 for [0, 1) offsets).
    };

    /// Whether XeSS was asked for and the device was created with its requirements.
    [[nodiscard]] static bool IsWanted();

    /// Throws vk::Exception when XeSS can't start: the runtime is missing or rejects the device.
    explicit XessUpscaler(const Device& device, Scheduler& scheduler,
                          MemoryAllocator& memory_allocator);
    ~XessUpscaler();

    /// Records one evaluation into the output image (RGBA16F at the upscale size, GENERAL
    /// layout). False when XeSS can't run (the caller then uses FSR 3.1 from now on).
    bool Upscale(const Inputs& inputs);

    [[nodiscard]] bool Failed() const noexcept {
        return failed.load();
    }
    [[nodiscard]] VkImageView OutputView() const noexcept {
        return *output_view;
    }

private:
    bool Init(VkExtent2D render_size, VkExtent2D upscale_size);
    void CreateOutput(VkExtent2D upscale_size);

    const Device& device;
    Scheduler& scheduler;
    MemoryAllocator& memory_allocator;
    std::shared_ptr<const XessRuntime::Api> api;
    void* context{}; ///< xess_context_handle_t
    VkExtent2D render_extent{};
    VkExtent2D upscale_extent{};
    vk::Image output_image;
    vk::ImageView output_view;
    std::atomic<bool> failed{};
};

} // namespace Vulkan
