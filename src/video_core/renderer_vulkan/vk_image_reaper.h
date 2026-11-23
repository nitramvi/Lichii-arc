// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

/// EDEN_NX2_IMAGE_REAPER: destroys deleted images' Vulkan objects on a thread of its own. Only
/// objects the GPU is done with are handed over.
class ImageReaper {
public:
    struct Objects {
        std::vector<vk::ImageView> views;
        vk::Image original;
        vk::Image scaled;
        vk::Buffer buffer;
        u64 bytes{};
    };

    explicit ImageReaper(u64 cap_bytes);
    ~ImageReaper();

    ImageReaper(const ImageReaper&) = delete;
    ImageReaper& operator=(const ImageReaper&) = delete;

    /// Takes the objects unless the queue already holds `cap_bytes`: then they are left with the
    /// caller, who destroys them where it is.
    bool Push(Objects&& objects);

    /// Destroys what is queued on the calling thread, so its memory is back now.
    void Drain();

private:
    void Run(std::stop_token token);
    void Report();

    const u64 cap_bytes;
    std::mutex mutex;
    std::condition_variable_any work_cv;
    std::deque<Objects> queue;
    u64 queued_bytes{}; ///< Queued and in the reaper's hands, until destroyed

    // Statistics, logged every 10 s while images are freed (guarded by mutex).
    u64 reaped{};
    u64 reaped_bytes{};
    u64 refused{};
    u64 drained{};
    u64 max_queued_bytes{};
    u64 last_report_ns{};

    std::jthread thread; ///< Last: started once the rest is constructed, joined first
};

} // namespace Vulkan
