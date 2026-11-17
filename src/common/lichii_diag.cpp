// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <limits>
#include <span>
#include <string>
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
#include <winioctl.h>
#include <setupapi.h>
#include <batclass.h>
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "setupapi.lib")
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

// Minimal transcription of the Intel Graphics Control Library ABI (igcl_api.h, version 1.1) for
// ctlInit, ctlEnumerateDevices and ctlPowerTelemetryGet. ControlLib.dll ships with Intel's
// graphics driver and is loaded at run time; without it the GPU fields read n/a.
namespace Igcl {
struct TelemetryItem {
    bool supported;
    s32 units;
    s32 type;
    union {
        s8 i8;
        u8 u8v;
        s16 i16;
        u16 u16v;
        s32 i32;
        u32 u32v;
        s64 i64;
        u64 u64v;
        float f;
        double d;
    } value;
};
static_assert(sizeof(TelemetryItem) == 24);

struct PsuInfo {
    bool supported;
    s32 psu_type;
    TelemetryItem energy_counter;
    TelemetryItem voltage;
};

struct PowerTelemetry {
    u32 size;
    u8 version;
    TelemetryItem time_stamp;
    TelemetryItem gpu_energy_counter;
    TelemetryItem gpu_voltage;
    TelemetryItem gpu_current_clock_frequency;
    TelemetryItem gpu_current_temperature;
    TelemetryItem global_activity_counter;
    TelemetryItem render_compute_activity_counter;
    TelemetryItem media_activity_counter;
    bool gpu_power_limited;
    bool gpu_temperature_limited;
    bool gpu_current_limited;
    bool gpu_voltage_limited;
    bool gpu_utilization_limited;
    TelemetryItem vram_items[7];
    bool vram_limited[5];
    TelemetryItem total_card_energy_counter;
    PsuInfo psu[5];
    TelemetryItem fan_speed[5];
    TelemetryItem tail[9];
    u8 reserve[512]; // Room for fields newer drivers may append.
};

struct InitArgs {
    u32 size;
    u8 version;
    u32 app_version;
    u32 flags;
    u32 supported_version;
    u32 uid[4];
};

using PfnInit = s32(__cdecl*)(InitArgs*, void**);
using PfnEnumerate = s32(__cdecl*)(void*, u32*, void**);
using PfnTelemetry = s32(__cdecl*)(void*, PowerTelemetry*);

double Value(const TelemetryItem& item) {
    switch (item.type) {
    case 0: return item.value.i8;
    case 1: return item.value.u8v;
    case 2: return item.value.i16;
    case 3: return item.value.u16v;
    case 4: return item.value.i32;
    case 5: return item.value.u32v;
    case 6: return static_cast<double>(item.value.i64);
    case 7: return static_cast<double>(item.value.u64v);
    case 8: return item.value.f;
    case 9: return item.value.d;
    default: return 0.0;
    }
}

class Gpu {
public:
    Gpu() {
        HMODULE lib = LoadLibraryExW(L"ControlLib.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!lib) {
            return;
        }
        const auto init = reinterpret_cast<PfnInit>(GetProcAddress(lib, "ctlInit"));
        const auto enumerate =
            reinterpret_cast<PfnEnumerate>(GetProcAddress(lib, "ctlEnumerateDevices"));
        telemetry = reinterpret_cast<PfnTelemetry>(GetProcAddress(lib, "ctlPowerTelemetryGet"));
        if (!init || !enumerate || !telemetry) {
            return;
        }
        InitArgs args{};
        args.size = sizeof(args);
        args.app_version = (1u << 16) | 1u;
        args.flags = 1; // CTL_INIT_FLAG_USE_LEVEL_ZERO: telemetry needs it.
        void* api = nullptr;
        if (init(&args, &api) != 0 || !api) {
            return;
        }
        u32 count = 0;
        if (enumerate(api, &count, nullptr) != 0 || count == 0) {
            return;
        }
        std::vector<void*> devices(count);
        if (enumerate(api, &count, devices.data()) != 0) {
            return;
        }
        for (void* candidate : devices) {
            for (u8 try_version : {u8{0}, u8{1}}) {
                version = try_version;
                PowerTelemetry sample{};
                if (Read(candidate, sample)) {
                    device = candidate;
                    previous = sample;
                    return;
                }
            }
        }
    }

    bool Available() const {
        return device != nullptr;
    }

    struct Reading {
        double mhz = -1, watts = -1, temp_c = -1, render_pct = -1, global_pct = -1;
        bool power_limited = false, temperature_limited = false;
    };

    Reading Sample() {
        Reading reading;
        PowerTelemetry sample{};
        if (!device || !Read(device, sample)) {
            return reading;
        }
        const double dt = Value(sample.time_stamp) - Value(previous.time_stamp);
        if (sample.gpu_current_clock_frequency.supported) {
            reading.mhz = Value(sample.gpu_current_clock_frequency);
        }
        if (sample.gpu_current_temperature.supported) {
            reading.temp_c = Value(sample.gpu_current_temperature);
        }
        if (dt > 0) {
            const auto rate = [&](const TelemetryItem& now, const TelemetryItem& before) {
                return now.supported ? (Value(now) - Value(before)) / dt : -1.0;
            };
            reading.watts = rate(sample.gpu_energy_counter, previous.gpu_energy_counter);
            const double render = rate(sample.render_compute_activity_counter,
                                       previous.render_compute_activity_counter);
            const double global =
                rate(sample.global_activity_counter, previous.global_activity_counter);
            reading.render_pct = render < 0 ? -1.0 : std::clamp(render * 100.0, 0.0, 100.0);
            reading.global_pct = global < 0 ? -1.0 : std::clamp(global * 100.0, 0.0, 100.0);
        }
        reading.power_limited = sample.gpu_power_limited;
        reading.temperature_limited = sample.gpu_temperature_limited;
        previous = sample;
        return reading;
    }

private:
    bool Read(void* handle, PowerTelemetry& sample) const {
        sample = {};
        sample.size = sizeof(PowerTelemetry) - sizeof(sample.reserve);
        sample.version = version;
        return telemetry(handle, &sample) == 0;
    }

    PfnTelemetry telemetry{};
    void* device{};
    u8 version{};
    PowerTelemetry previous{};
};
} // namespace Igcl

const char* PowerModeName() {
    using Pfn = DWORD(WINAPI*)(GUID*);
    static const Pfn get = [] {
        HMODULE lib = LoadLibraryExW(L"powrprof.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return lib ? reinterpret_cast<Pfn>(GetProcAddress(lib, "PowerGetEffectiveOverlayScheme"))
                   : nullptr;
    }();
    GUID mode{};
    if (!get || get(&mode) != ERROR_SUCCESS) {
        return "n/a";
    }
    static constexpr GUID best_efficiency{
        0x961cc777, 0x2547, 0x4f9d, {0x81, 0x74, 0x7d, 0x86, 0x18, 0x1b, 0x8a, 0x7a}};
    static constexpr GUID better_battery{
        0x3af9b8d9, 0x7c97, 0x431d, {0xad, 0x78, 0x34, 0xa8, 0xbf, 0xea, 0x43, 0x9f}};
    static constexpr GUID best_performance{
        0xded574b5, 0x45a0, 0x4f42, {0x87, 0x37, 0x46, 0x34, 0x5c, 0x09, 0xc2, 0x38}};
    if (IsEqualGUID(mode, best_efficiency)) {
        return "best_efficiency";
    }
    if (IsEqualGUID(mode, better_battery)) {
        return "better_battery";
    }
    if (IsEqualGUID(mode, best_performance)) {
        return "best_performance";
    }
    if (IsEqualGUID(mode, GUID{})) {
        return "balanced";
    }
    return "other";
}

/// The first battery's charge (+) or discharge (-) rate in watts; NaN when unknown.
class Battery {
public:
    Battery() {
        static constexpr GUID battery_class{
            0x72631e54, 0x78a4, 0x11d0, {0xbc, 0xf7, 0x00, 0xaa, 0x00, 0xb7, 0xb3, 0x2a}};
        HDEVINFO set = SetupDiGetClassDevsW(&battery_class, nullptr, nullptr,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (set == INVALID_HANDLE_VALUE) {
            return;
        }
        SP_DEVICE_INTERFACE_DATA data{};
        data.cbSize = sizeof(data);
        if (SetupDiEnumDeviceInterfaces(set, nullptr, &battery_class, 0, &data)) {
            DWORD size = 0;
            SetupDiGetDeviceInterfaceDetailW(set, &data, nullptr, 0, &size, nullptr);
            std::vector<u8> buffer(
                std::max<size_t>(size, sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)));
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (SetupDiGetDeviceInterfaceDetailW(set, &data, detail, size, &size, nullptr)) {
                handle = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
            }
        }
        SetupDiDestroyDeviceInfoList(set);
    }

    ~Battery() {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }

    double RateWatts() const {
        if (handle == INVALID_HANDLE_VALUE) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        DWORD wait = 0, bytes = 0;
        BATTERY_WAIT_STATUS query{};
        if (!DeviceIoControl(handle, IOCTL_BATTERY_QUERY_TAG, &wait, sizeof(wait),
                             &query.BatteryTag, sizeof(query.BatteryTag), &bytes, nullptr) ||
            query.BatteryTag == 0) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        BATTERY_STATUS status{};
        if (!DeviceIoControl(handle, IOCTL_BATTERY_QUERY_STATUS, &query, sizeof(query), &status,
                             sizeof(status), &bytes, nullptr) ||
            status.Rate == static_cast<LONG>(BATTERY_UNKNOWN_RATE)) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        return status.Rate / 1000.0;
    }

private:
    HANDLE handle = INVALID_HANDLE_VALUE;
};

std::string Num(double value, int digits = 0) {
    return value < 0 || std::isnan(value) ? std::string{"n/a"}
                                          : fmt::format("{:.{}f}", value, digits);
}

struct PdhArray {
    std::vector<u8> storage;
    std::span<const PDH_FMT_COUNTERVALUE_ITEM_W> items;
};

bool ReadArray(PDH_HCOUNTER counter, PdhArray& out) {
    if (!counter) {
        return false;
    }
    DWORD size = 0, count = 0;
    PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count,
                                 nullptr);
    if (size == 0) {
        return false;
    }
    out.storage.resize(size);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(out.storage.data());
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count,
                                     items) != ERROR_SUCCESS) {
        return false;
    }
    out.items = {items, count};
    return true;
}

/// "group,index" Processor Information instance -> logical processor; -1 for totals.
int ProcessorIndex(const wchar_t* name) {
    if (!name || std::wcsstr(name, L"_Total")) {
        return -1;
    }
    const wchar_t* comma = std::wcschr(name, L',');
    if (!comma) {
        return -1;
    }
    return static_cast<int>(std::wcstol(name, nullptr, 10) * 64 + std::wcstol(comma + 1, nullptr, 10));
}

struct TelemetryWindow {
    u32 samples = 0;
    u32 gpu_samples = 0;
    u32 battery_samples = 0;
    double cpu_p_mhz = 0, cpu_e_mhz = 0, gpu_mhz = 0, gpu_w = 0, gpu_temp = 0, gpu_temp_max = 0;
    double battery_w = 0, zone_temp_max = -1;
};
TelemetryWindow g_window;

void TelemetryThread() {
    const std::vector<BYTE> classes = EfficiencyClasses();
    const BYTE fastest =
        classes.empty() ? 0 : *std::max_element(classes.begin(), classes.end());
    const bool hybrid =
        !classes.empty() && *std::min_element(classes.begin(), classes.end()) != fastest;
    const auto is_performance = [&](int cpu) {
        return cpu < 0 || static_cast<size_t>(cpu) >= classes.size() || classes[cpu] == fastest;
    };
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const u32 cpus = system.dwNumberOfProcessors;

    PDH_HQUERY query{};
    PDH_HCOUNTER gpu_counter{}, limit_counter{}, perf_counter{}, freq_counter{}, utility_counter{},
        zone_counter{};
    const bool pdh = PdhOpenQueryW(nullptr, 0, &query) == ERROR_SUCCESS;
    if (pdh) {
        const auto add = [&](const wchar_t* path, PDH_HCOUNTER& counter) {
            if (PdhAddEnglishCounterW(query, path, 0, &counter) != ERROR_SUCCESS) {
                counter = nullptr;
            }
        };
        // Wildcard counters: every process's 3D engine instance, summed below.
        add(L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", gpu_counter);
        add(L"\\Processor Information(_Total)\\% Performance Limit", limit_counter);
        // Real clock = base clock ("Processor Frequency") x "% Processor Performance".
        add(L"\\Processor Information(*)\\% Processor Performance", perf_counter);
        add(L"\\Processor Information(*)\\Processor Frequency", freq_counter);
        add(L"\\Processor Information(*)\\% Processor Utility", utility_counter);
        // ACPI thermal zones (Kelvin): the only temperature Windows gives without a driver.
        add(L"\\Thermal Zone Information(*)\\Temperature", zone_counter);
        PdhCollectQueryData(query);
    }
    Igcl::Gpu igcl;
    Battery battery;
    LOG_INFO(Common,
             "LICHII_DIAG telemetry: {} logical processors, efficiency classes {}..{}, igcl={}, "
             "pdh_cpu_perf={}, thermal_zones={}",
             cpus, classes.empty() ? 0 : *std::min_element(classes.begin(), classes.end()),
             fastest, igcl.Available() ? "yes" : "no", perf_counter ? "yes" : "no",
             zone_counter ? "yes" : "no");
    std::vector<ProcessorPowerInformation> power(cpus);
    PdhArray perf_items, freq_items, utility_items, array;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds{2});
        u64 p_max = 0, e_max = 0;
        u32 limited = 0;
        if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, power.data(),
                                   static_cast<ULONG>(power.size() *
                                                      sizeof(ProcessorPowerInformation))) == 0) {
            for (u32 i = 0; i < cpus; ++i) {
                const bool performance = is_performance(static_cast<int>(i));
                (performance ? p_max : e_max) =
                    std::max<u64>(performance ? p_max : e_max, power[i].max_mhz);
                limited += power[i].mhz_limit < power[i].max_mhz ? 1 : 0;
            }
        }
        double gpu_util = -1.0, perf_limit = -1.0, zone_max = -1.0;
        double p_mhz = -1.0, e_mhz = -1.0, p_util = -1.0, e_util = -1.0;
        if (pdh && PdhCollectQueryData(query) == ERROR_SUCCESS) {
            if (ReadArray(gpu_counter, array)) {
                gpu_util = 0.0;
                for (const auto& item : array.items) {
                    gpu_util += item.FmtValue.doubleValue;
                }
                gpu_util = std::min(gpu_util, 100.0);
            }
            if (limit_counter) {
                PDH_FMT_COUNTERVALUE value{};
                if (PdhGetFormattedCounterValue(limit_counter, PDH_FMT_DOUBLE, nullptr, &value) ==
                    ERROR_SUCCESS) {
                    perf_limit = value.doubleValue;
                }
            }
            if (ReadArray(perf_counter, perf_items) && ReadArray(freq_counter, freq_items)) {
                std::vector<double> base(cpus + 64, 0.0);
                for (const auto& item : freq_items.items) {
                    const int cpu = ProcessorIndex(item.szName);
                    if (cpu >= 0 && static_cast<size_t>(cpu) < base.size()) {
                        base[cpu] = item.FmtValue.doubleValue;
                    }
                }
                double sum[2]{}, count[2]{};
                for (const auto& item : perf_items.items) {
                    const int cpu = ProcessorIndex(item.szName);
                    if (cpu < 0 || static_cast<size_t>(cpu) >= base.size() || base[cpu] <= 0) {
                        continue;
                    }
                    const int kind = is_performance(cpu) ? 0 : 1;
                    sum[kind] += base[cpu] * item.FmtValue.doubleValue / 100.0;
                    count[kind] += 1;
                }
                p_mhz = count[0] ? sum[0] / count[0] : -1.0;
                e_mhz = count[1] ? sum[1] / count[1] : -1.0;
            }
            if (ReadArray(utility_counter, utility_items)) {
                double sum[2]{}, count[2]{};
                for (const auto& item : utility_items.items) {
                    const int cpu = ProcessorIndex(item.szName);
                    if (cpu < 0) {
                        continue;
                    }
                    const int kind = is_performance(cpu) ? 0 : 1;
                    sum[kind] += item.FmtValue.doubleValue;
                    count[kind] += 1;
                }
                p_util = count[0] ? sum[0] / count[0] : -1.0;
                e_util = count[1] ? sum[1] / count[1] : -1.0;
            }
            if (ReadArray(zone_counter, array)) {
                for (const auto& item : array.items) {
                    if (item.FmtValue.doubleValue > 200.0) {
                        zone_max = std::max(zone_max, item.FmtValue.doubleValue - 273.15);
                    }
                }
            }
        }
        const Igcl::Gpu::Reading gpu = igcl.Sample();
        SYSTEM_POWER_STATUS status{};
        const bool have_status = GetSystemPowerStatus(&status) != 0;
        const bool on_ac = have_status && status.ACLineStatus == 1;
        const int battery_pct =
            have_status && status.BatteryLifePercent <= 100 ? status.BatteryLifePercent : -1;
        const double battery_w = battery.RateWatts();
        // perf_limit: Windows' "% Performance Limit" (100 = no limit; lower = throttled by
        // power or temperature). cpu_*_mhz: real clocks; cpu_*_max_mhz: rated maximum.
        LOG_INFO(Common,
                 "LICHII_TELEMETRY cpu_p_mhz={} cpu_e_mhz={} cpu_p_max_mhz={} cpu_e_max_mhz={} "
                 "cpu_p_util_pct={} cpu_e_util_pct={} hybrid={} cores_limited={} "
                 "perf_limit_pct={} gpu_3d_util_pct={} gpu_mhz={} gpu_w={} gpu_temp_c={} "
                 "gpu_render_pct={} gpu_busy_pct={} gpu_power_limited={} gpu_temp_limited={} "
                 "thermal_zone_max_c={} power_mode={} power_source={} battery_pct={} "
                 "battery_w={} throttling={}",
                 Num(p_mhz), Num(e_mhz), p_max, e_max, Num(p_util), Num(e_util),
                 hybrid ? "yes" : "no", limited, Num(perf_limit), Num(gpu_util), Num(gpu.mhz),
                 Num(gpu.watts, 1), Num(gpu.temp_c), Num(gpu.render_pct), Num(gpu.global_pct),
                 igcl.Available() ? (gpu.power_limited ? "yes" : "no") : "n/a",
                 igcl.Available() ? (gpu.temperature_limited ? "yes" : "no") : "n/a",
                 Num(zone_max), PowerModeName(),
                 !have_status ? "n/a" : (on_ac ? "ac" : "battery"),
                 battery_pct < 0 ? std::string{"n/a"} : std::to_string(battery_pct),
                 std::isnan(battery_w) ? std::string{"n/a"} : fmt::format("{:.1f}", battery_w),
                 (perf_limit >= 0 && perf_limit < 95.0) || limited != 0 ||
                         gpu.power_limited || gpu.temperature_limited
                     ? "yes"
                     : "no");
        std::scoped_lock lock{g_mutex};
        ++g_window.samples;
        g_window.cpu_p_mhz += std::max(p_mhz, 0.0);
        g_window.cpu_e_mhz += std::max(e_mhz, 0.0);
        if (gpu.mhz >= 0 && gpu.watts >= 0) {
            ++g_window.gpu_samples;
            g_window.gpu_mhz += gpu.mhz;
            g_window.gpu_w += gpu.watts;
            g_window.gpu_temp += std::max(gpu.temp_c, 0.0);
            g_window.gpu_temp_max = std::max(g_window.gpu_temp_max, gpu.temp_c);
        }
        if (!std::isnan(battery_w)) {
            ++g_window.battery_samples;
            g_window.battery_w += battery_w;
        }
        g_window.zone_temp_max = std::max(g_window.zone_temp_max, zone_max);
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

std::string TakeTelemetrySummary() {
#ifdef _WIN32
    std::scoped_lock lock{g_mutex};
    const TelemetryWindow window = std::exchange(g_window, TelemetryWindow{});
    if (window.samples == 0) {
        return "telemetry=none";
    }
    const auto avg = [](double sum, u32 n) { return n ? Num(sum / n, 1) : std::string{"n/a"}; };
    return fmt::format("cpu_p_mhz={} cpu_e_mhz={} gpu_mhz={} gpu_w={} gpu_temp_c={} "
                       "gpu_temp_max_c={} thermal_zone_max_c={} battery_w={} samples={}",
                       avg(window.cpu_p_mhz, window.samples), avg(window.cpu_e_mhz, window.samples),
                       avg(window.gpu_mhz, window.gpu_samples), avg(window.gpu_w, window.gpu_samples),
                       avg(window.gpu_temp, window.gpu_samples),
                       window.gpu_samples ? Num(window.gpu_temp_max) : std::string{"n/a"},
                       Num(window.zone_temp_max),
                       window.battery_samples
                           ? fmt::format("{:.1f}", window.battery_w / window.battery_samples)
                           : std::string{"n/a"},
                       window.samples);
#else
    return "telemetry=none";
#endif
}

} // namespace Common::LichiiDiag
