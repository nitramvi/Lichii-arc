Intel XeSS SDK public headers (MIT, see the SPDX lines in each header), from
https://github.com/intel/xess (commit de0fb9c). Only the headers are vendored: the XeSS runtime
(libxess.dll, Intel Simplified Software License) is loaded at run time from the emulator's folder
when present (src/video_core/vulkan_common/xess_runtime.cpp).
