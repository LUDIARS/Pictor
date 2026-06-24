#include "pictor/core/cache_info.h"

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <vector>
#elif defined(__APPLE__)
  #include <sys/sysctl.h>
#else // Linux / Android
  #include <unistd.h>
  #include <cstdio>
#endif

namespace pictor {

namespace {

const char* detect_arch() {
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    return "arm64-apple";
#elif defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm32";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86-64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#elif defined(__powerpc64__) || defined(__ppc64__)
    return "ppc64";
#else
    return "unknown";
#endif
}

#if defined(_WIN32)

size_t detect_runtime_line_size(const char** source) {
    *source = "win32-logical-processor";
    DWORD len = 0;
    GetLogicalProcessorInformation(nullptr, &len);
    if (len == 0) return 0;

    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> buf(
        len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    if (!GetLogicalProcessorInformation(buf.data(), &len)) return 0;

    // L1 のデータ / 統合キャッシュのライン幅を採る。
    for (const auto& info : buf) {
        if (info.Relationship == RelationCache &&
            info.Cache.Level == 1 &&
            (info.Cache.Type == CacheData || info.Cache.Type == CacheUnified)) {
            return static_cast<size_t>(info.Cache.LineSize);
        }
    }
    return 0;
}

#elif defined(__APPLE__)

size_t detect_runtime_line_size(const char** source) {
    *source = "sysctl-hw.cachelinesize";
    size_t line = 0;
    size_t sz = sizeof(line);
    if (sysctlbyname("hw.cachelinesize", &line, &sz, nullptr, 0) == 0) {
        return line;
    }
    return 0;
}

#else // Linux / Android

size_t detect_runtime_line_size(const char** source) {
    *source = "sysconf";
#if defined(_SC_LEVEL1_DCACHE_LINESIZE)
    long v = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    if (v > 0) return static_cast<size_t>(v);
#endif
    // フォールバック: sysfs の coherency_line_size を読む。
    *source = "sysfs-coherency_line_size";
    if (FILE* f = std::fopen(
            "/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size", "r")) {
        long v = 0;
        const int n = std::fscanf(f, "%ld", &v);
        std::fclose(f);
        if (n == 1 && v > 0) return static_cast<size_t>(v);
    }
    return 0;
}

#endif

} // namespace

CacheLineInfo query_cache_line_info() {
    CacheLineInfo info;
    info.compiled_line_size = PICTOR_CACHE_LINE_SIZE;
    info.arch = detect_arch();

    const char* source = "none";
    info.runtime_line_size = detect_runtime_line_size(&source);
    info.source = source;

    // 検出不能 (0) は「不明」扱いで一致とみなす (誤警告を出さない)。
    info.matches = (info.runtime_line_size == 0) ||
                   (info.runtime_line_size == info.compiled_line_size);
    return info;
}

} // namespace pictor
