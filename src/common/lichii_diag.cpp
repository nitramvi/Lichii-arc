// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <pdh.h>
#include <powerbase.h>
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "powrprof.lib")
#endif

#include "common/lichii_diag.h"
#include "common/logging.h"

namespace Common::LichiiDiag {
namespace {
std::atomic<u32> g_marks{};
std::atomic<u32> g_pending_mark{};
std::atomic<u32> g_ring_dump{};

std::mutex g_mutex;
std::string g_last_file;
std::chrono::steady_clock::time_point g_last_file_time{};
bool g_any_file{};
std::string g_events;
double g_gpu_sum{};
double g_gpu_max{};
u32 g_gpu_samples{};

std::once_flag g_telemetry_once;

#ifdef _WIN32
// CallNtPowerInformation(ProcessorInformation)'s record (not in the SDK headers).
struct ProcessorPowerInformation {
    ULONG number;
    ULONG max_mhz;
    ULONG current_mhz;
    ULONG mhz_limit;
    ULONG max_idle_state;
    ULONG current_idle_state;
};

/// Each logical processor's efficiency class (0: the most efficient cores, higher: faster).
std::vector<BYTE> EfficiencyClasses() {
    std::vector<BYTE> classes;
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    if (length == 0) {
        return classes;
    }
    std::vector<u8> buffer(length);
    auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info, &length)) {
        return classes;
    }
    for (DWORD offset = 0; offset < length;) {
        auto* entry = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() +
                                                                                 offset);
        const auto& processor = entry->Processor;
        for (WORD group = 0; group < processor.GroupCount; ++group) {
            KAFFINITY mask = processor.GroupMask[group].Mask;
            for (u32 bit = 0; mask != 0; ++bit, mask >>= 1) {
                if ((mask & 1) == 0) {
                    continue;
                }
                const size_t index = group * 64 + bit;
                if (classes.size() <= index) {
                    classes.resize(index + 1, 0);
                }
                classes[index] = processor.EfficiencyClass;
            }
        }
        offset += entry->Size;
    }
    return classes;
}

void TelemetryThread() {
    const std::vector<BYTE> classes = EfficiencyClasses();
    const BYTE fastest =
        classes.empty() ? 0 : *std::max_element(classes.begin(), classes.end());
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const u32 cpus = system.dwNumberOfProcessors;

    PDH_HQUERY query{};
    PDH_HCOUNTER gpu_counter{};
    PDH_HCOUNTER limit_counter{};
    bool pdh = PdhOpenQueryW(nullptr, 0, &query) == ERROR_SUCCESS;
    if (pdh) {
        // Wildcard counters: every process's 3D engine instance, summed below.
        if (PdhAddEnglishCounterW(query, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0,
                                  &gpu_counter) != ERROR_SUCCESS) {
            gpu_counter = nullptr;
        }
        if (PdhAddEnglishCounterW(query,
                                  L"\\Processor Information(_Total)\\% Performance Limit", 0,
                                  &limit_counter) != ERROR_SUCCESS) {
            limit_counter = nullptr;
        }
        PdhCollectQueryData(query);
    }
    LOG_INFO(Common, "LICHII_DIAG telemetry: {} logical processors, efficiency classes {}..{}",
             cpus, classes.empty() ? 0 : *std::min_element(classes.begin(), classes.end()),
             fastest);
    std::vector<ProcessorPowerInformation> power(cpus);
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds{2});
        u64 p_sum = 0, e_sum = 0, p_max = 0, e_max = 0;
        u32 p_count = 0, e_count = 0;
        u32 limited = 0;
        if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, power.data(),
                                   static_cast<ULONG>(power.size() *
                                                      sizeof(ProcessorPowerInformation))) == 0) {
            for (u32 i = 0; i < cpus; ++i) {
                const bool performance = classes.size() <= i || classes[i] == fastest;
                (performance ? p_sum : e_sum) += power[i].current_mhz;
                (performance ? p_count : e_count) += 1;
                (performance ? p_max : e_max) =
                    std::max<u64>(performance ? p_max : e_max, power[i].max_mhz);
                limited += power[i].mhz_limit < power[i].max_mhz ? 1 : 0;
            }
        }
        double gpu_util = -1.0;
        double perf_limit = -1.0;
        if (pdh && PdhCollectQueryData(query) == ERROR_SUCCESS) {
            if (gpu_counter) {
                DWORD size = 0, count = 0;
                PdhGetFormattedCounterArrayW(gpu_counter, PDH_FMT_DOUBLE, &size, &count, nullptr);
                std::vector<u8> items(size);
                if (size != 0 &&
                    PdhGetFormattedCounterArrayW(
                        gpu_counter, PDH_FMT_DOUBLE, &size, &count,
                        reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(items.data())) ==
                        ERROR_SUCCESS) {
                    const auto* item =
                        reinterpret_cast<const PDH_FMT_COUNTERVALUE_ITEM_W*>(items.data());
                    gpu_util = 0.0;
                    for (DWORD i = 0; i < count; ++i) {
                        gpu_util += item[i].FmtValue.doubleValue;
                    }
                    gpu_util = std::min(gpu_util, 100.0);
                }
            }
            if (limit_counter) {
                PDH_FMT_COUNTERVALUE value{};
                if (PdhGetFormattedCounterValue(limit_counter, PDH_FMT_DOUBLE, nullptr, &value) ==
                    ERROR_SUCCESS) {
                    perf_limit = value.doubleValue;
                }
            }
        }
        // perf_limit: Windows' "% Performance Limit" (100 = no limit; lower = throttled by
        // power or temperature). Temperature itself needs vendor tools or administrator rights.
        LOG_INFO(Common,
                 "LICHII_TELEMETRY cpu_p_mhz={}/{} cpu_e_mhz={}/{} cores_limited={} "
                 "perf_limit_pct={} gpu_3d_util_pct={} throttling={}",
                 p_count ? p_sum / p_count : 0, p_max, e_count ? e_sum / e_count : 0, e_max,
                 limited, perf_limit < 0 ? std::string{"n/a"} : fmt::format("{:.0f}", perf_limit),
                 gpu_util < 0 ? std::string{"n/a"} : fmt::format("{:.0f}", gpu_util),
                 (perf_limit >= 0 && perf_limit < 95.0) || limited != 0 ? "yes" : "no");
    }
}
#endif
} // Anonymous namespace

bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("LICHII_DIAG");
        return value && value[0] == '1';
    }();
    return enabled;
}

void StartTelemetry() {
#ifdef _WIN32
    if (!Enabled()) {
        return;
    }
    std::call_once(g_telemetry_once, [] {
        std::thread{[] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
            TelemetryThread();
        }}.detach();
    });
#endif
}

u32 Mark() {
    if (!Enabled()) {
        return 0;
    }
    const u32 number = g_marks.fetch_add(1, std::memory_order_relaxed) + 1;
    g_pending_mark.store(number, std::memory_order_relaxed);
    g_ring_dump.store(number, std::memory_order_relaxed);
    LOG_INFO(Common, "LICHII_MARK {} requested (last file: {})", number, LastFileOpened());
    return number;
}

bool RingEnabled() {
    static const bool enabled = [] {
        if (!Enabled()) {
            return false;
        }
        const char* value = std::getenv("LICHII_DIAG_RING");
        return !(value && value[0] == '0');
    }();
    return enabled;
}

double RingSeconds() {
    static const double seconds = [] {
        const char* value = std::getenv("LICHII_DIAG_RING_SECONDS");
        const double parsed = value ? std::atof(value) : 3.0;
        return parsed >= 1.0 && parsed <= 10.0 ? parsed : 3.0;
    }();
    return seconds;
}

u32 TakeRingDump() {
    return g_ring_dump.exchange(0, std::memory_order_relaxed);
}

u32 TakePendingMark() {
    return g_pending_mark.exchange(0, std::memory_order_relaxed);
}

void NoteFileOpened(std::string_view path) {
    if (!Enabled()) {
        return;
    }
    std::scoped_lock lock{g_mutex};
    g_last_file.assign(path);
    g_last_file_time = std::chrono::steady_clock::now();
    g_any_file = true;
}

std::string LastFileOpened() {
    std::scoped_lock lock{g_mutex};
    return g_any_file ? g_last_file : std::string{"none"};
}

double SecondsSinceFileOpened() {
    std::scoped_lock lock{g_mutex};
    if (!g_any_file) {
        return 1e9;
    }
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_last_file_time)
        .count();
}

void NoteEvent(std::string_view event) {
    if (!Enabled()) {
        return;
    }
    std::scoped_lock lock{g_mutex};
    if (!g_events.empty()) {
        g_events += ',';
    }
    g_events.append(event);
}

std::string TakeEvents() {
    std::scoped_lock lock{g_mutex};
    return std::exchange(g_events, std::string{});
}

void AddGpuFrameSample(double busy_ms) {
    if (!Enabled()) {
        return;
    }
    std::scoped_lock lock{g_mutex};
    g_gpu_sum += busy_ms;
    g_gpu_max = std::max(g_gpu_max, busy_ms);
    ++g_gpu_samples;
}

std::string TakeGpuSummary() {
    std::scoped_lock lock{g_mutex};
    if (g_gpu_samples == 0) {
        return "gpu_ms=none";
    }
    const std::string text = fmt::format("gpu_ms=avg:{:.1f},max:{:.1f},samples:{}",
                                         g_gpu_sum / g_gpu_samples, g_gpu_max, g_gpu_samples);
    g_gpu_sum = 0.0;
    g_gpu_max = 0.0;
    g_gpu_samples = 0;
    return text;
}

} // namespace Common::LichiiDiag
