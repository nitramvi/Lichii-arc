#!/usr/bin/env python3
"""lichii-arc patch for fsr4-vk (GPL-3.0, same license as fsr4-vk).

1. Builds the model's compute pipelines while the context is created (eagerly) instead of on the
   first upscaled frame, so a host that creates the context on a worker thread never stalls the
   game for the seconds the network's pipelines take.
2. Keeps those pipelines in a VkPipelineCache saved to the file in FSR4_VK_PIPELINE_CACHE (set by
   Lichii next to fsr4-vk.dll), so later sessions build them from the cache.
"""
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
path = root / "src/runtime/context.cpp"
src = path.read_text()


def sub(old, new):
    global src
    if src.count(old) != 1:
        sys.exit(f"patch_pipelines: anchor not found exactly once: {old[:70]!r}")
    src = src.replace(old, new)


helpers = r'''
namespace {
// lichii-arc: process-wide pipeline cache backed by the file in FSR4_VK_PIPELINE_CACHE.
std::mutex lichii_cache_mutex;
VkPipelineCache lichii_cache = VK_NULL_HANDLE;
VkDevice lichii_cache_device = VK_NULL_HANDLE;

std::filesystem::path LichiiCachePath() {
    const char* value = std::getenv("FSR4_VK_PIPELINE_CACHE");
    return value && value[0] ? std::filesystem::path{value} : std::filesystem::path{};
}

VkPipelineCache LichiiCache(VkDevice device) {
    std::scoped_lock lock{lichii_cache_mutex};
    if (lichii_cache != VK_NULL_HANDLE && lichii_cache_device == device) {
        return lichii_cache;
    }
    const auto file = LichiiCachePath();
    if (file.empty()) {
        return VK_NULL_HANDLE;
    }
    std::vector<char> data;
    if (std::ifstream in{file, std::ios::binary}) {
        data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    info.initialDataSize = data.size();
    info.pInitialData = data.empty() ? nullptr : data.data();
    VkPipelineCache cache = VK_NULL_HANDLE;
    if (vkCreatePipelineCache(device, &info, nullptr, &cache) != VK_SUCCESS) {
        info.initialDataSize = 0; // a stale or foreign cache: start empty
        info.pInitialData = nullptr;
        if (vkCreatePipelineCache(device, &info, nullptr, &cache) != VK_SUCCESS) {
            return VK_NULL_HANDLE;
        }
    }
    lichii_cache = cache;
    lichii_cache_device = device;
    return cache;
}

void LichiiSaveCache(VkDevice device) {
    std::scoped_lock lock{lichii_cache_mutex};
    const auto file = LichiiCachePath();
    if (lichii_cache == VK_NULL_HANDLE || lichii_cache_device != device || file.empty()) {
        return;
    }
    size_t size = 0;
    if (vkGetPipelineCacheData(device, lichii_cache, &size, nullptr) != VK_SUCCESS || size == 0) {
        return;
    }
    std::vector<char> data(size);
    if (vkGetPipelineCacheData(device, lichii_cache, &size, data.data()) != VK_SUCCESS) {
        return;
    }
    const auto temp = std::filesystem::path{file}.concat(".tmp");
    {
        std::ofstream out{temp, std::ios::binary | std::ios::trunc};
        if (!out) {
            return;
        }
        out.write(data.data(), static_cast<std::streamsize>(size));
    }
    std::error_code ec;
    std::filesystem::rename(temp, file, ec);
}
} // namespace
'''

# Helpers right before the first QualityContext member definition.
anchor = "QualityContext::QualityContext(VkPhysicalDevice physical_device_,"
sub(anchor, helpers + "\n" + anchor)
src = ("#include <cstdlib>\n#include <filesystem>\n#include <fstream>\n#include <iterator>\n"
       "#include <mutex>\n#include <vector>\n") + src

sub("vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);",
    "vkCreateComputePipelines(device, LichiiCache(device), 1, &info, nullptr, &pipeline);")

# Eager model pipelines at the end of the constructor (the preset is already chosen there).
sub('''        writeAllStaticDescriptors();

    } catch (...) {''', '''        writeAllStaticDescriptors();

        // lichii-arc: build the network's pipelines now, not on the first upscaled frame.
        prepareModelPipelines();
        LichiiSaveCache(device);

    } catch (...) {''')

# Pipelines built later (a preset change) also go to the file.
sub('''        const auto code = readModelSpirv(paths[index], color_api_version);
        const auto pipeline = createComputePipeline(code, plan.require_wave32[index]);''',
    '''        const auto code = readModelSpirv(paths[index], color_api_version);
        const auto pipeline = createComputePipeline(code, plan.require_wave32[index]);
        ++lichii_new_pipelines;''')
sub('''    unsigned created = 0;
    for (uint32_t index = 0; index < paths.size(); ++index) {''', '''    unsigned created = 0;
    unsigned lichii_new_pipelines = 0;
    for (uint32_t index = 0; index < paths.size(); ++index) {''')

path.write_text(src)

# Save after a preset change built pipelines: find the end of prepareModelPipelines.
src = path.read_text()
start = src.index("void QualityContext::prepareModelPipelines() {")
depth = 0
i = src.index("{", start)
while True:
    if src[i] == "{":
        depth += 1
    elif src[i] == "}":
        depth -= 1
        if depth == 0:
            break
    i += 1
src = src[:i] + "    if (lichii_new_pipelines != 0) {\n        LichiiSaveCache(device);\n    }\n" + src[i:]
path.write_text(src)
print("patch_pipelines: fsr4-vk patched (eager model pipelines + pipeline cache file)")
