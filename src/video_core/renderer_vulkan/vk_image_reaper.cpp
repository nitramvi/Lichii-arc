// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <utility>

#include "common/logging.h"
#include "common/thread.h"
#include "video_core/renderer_vulkan/vk_image_reaper.h"

namespace Vulkan {

namespace {
u64 NowNs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

void Destroy(ImageReaper::Objects& objects) {
    objects.views.clear();
    objects.scaled.reset();
    objects.original.reset();
    objects.buffer = vk::Buffer{};
}
} // namespace

ImageReaper::ImageReaper(u64 cap_bytes_) : cap_bytes{cap_bytes_} {
    thread = std::jthread([this](std::stop_token token) { Run(token); });
    LOG_INFO(Render_Vulkan, "Deleted images are destroyed on a thread of their own (queue up to {} MiB)",
             cap_bytes >> 20);
}

ImageReaper::~ImageReaper() {
    thread.request_stop();
    if (thread.joinable()) {
        thread.join();
    }
    // What is left is destroyed with the queue.
}

bool ImageReaper::Push(Objects&& objects) {
    {
        std::scoped_lock lock{mutex};
        if (queued_bytes != 0 && queued_bytes + objects.bytes > cap_bytes) {
            ++refused;
            return false;
        }
        queued_bytes += objects.bytes;
        max_queued_bytes = (std::max)(max_queued_bytes, queued_bytes);
        queue.push_back(std::move(objects));
    }
    work_cv.notify_one();
    return true;
}

void ImageReaper::Drain() {
    std::deque<Objects> taken;
    {
        std::scoped_lock lock{mutex};
        taken.swap(queue);
        drained += taken.size();
    }
    u64 bytes = 0;
    for (Objects& objects : taken) {
        bytes += objects.bytes;
        Destroy(objects);
    }
    std::scoped_lock lock{mutex};
    queued_bytes -= bytes;
}

void ImageReaper::Run(std::stop_token token) {
    Common::SetCurrentThreadName("VulkanImageReaper");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
    while (!token.stop_requested()) {
        Objects objects;
        {
            std::unique_lock lock{mutex};
            if (!work_cv.wait(lock, token, [this] { return !queue.empty(); })) {
                return;
            }
            objects = std::move(queue.front());
            queue.pop_front();
        }
        Destroy(objects);
        std::scoped_lock lock{mutex};
        queued_bytes -= objects.bytes;
        ++reaped;
        reaped_bytes += objects.bytes;
        Report();
    }
}

void ImageReaper::Report() {
    const u64 now = NowNs();
    if (last_report_ns == 0) {
        last_report_ns = now;
        return;
    }
    if (now - last_report_ns < 10'000'000'000ULL) {
        return;
    }
    LOG_INFO(Render_Vulkan,
             "Image reaper last {:.0f} s: {} images ({} MiB) destroyed, {} left to the GPU thread "
             "(queue full), {} drained for memory, queue at most {} MiB",
             static_cast<double>(now - last_report_ns) / 1e9, reaped, reaped_bytes >> 20, refused,
             drained, max_queued_bytes >> 20);
    last_report_ns = now;
    reaped = reaped_bytes = refused = drained = max_queued_bytes = 0;
}

} // namespace Vulkan
