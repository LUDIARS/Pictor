/// FileSystemResourceLoader — path API and configured-root containment.

#include "pictor/visus/resource_loader.h"
#include "test_common.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace pictor;
using namespace pictor_test;

namespace fs = std::filesystem;

int main() {
    std::error_code ec;
    const fs::path parent = fs::temp_directory_path(ec);
    const fs::path root = parent / "pictor_unit_visus_resource_root";
    const fs::path outside = parent / "pictor_unit_visus_resource_outside.bin";
    fs::remove_all(root, ec);
    fs::remove(outside, ec);
    fs::create_directories(root, ec);
    {
        std::ofstream safe(root / "safe.bin", std::ios::binary);
        safe.write("abc", 3);
        std::ofstream escaped(outside, std::ios::binary);
        escaped.write("xyz", 3);
    }

    FileSystemResourceLoader loader(root.generic_string());
    std::string error = "stale";
    const std::vector<uint8_t> safe = loader.fetch("safe.bin", &error);
    PT_ASSERT_OP(safe.size(), ==, size_t{3}, "relative file under root loads");
    PT_ASSERT(error.empty(), "successful fetch clears stale error");

    const std::vector<uint8_t> escaped = loader.fetch("../pictor_unit_visus_resource_outside.bin", &error);
    PT_ASSERT(escaped.empty(), "parent traversal is rejected");
    PT_ASSERT(error.find("escapes root") != std::string::npos, "traversal error is explicit");

    const std::vector<uint8_t> absolute = loader.fetch(outside.generic_string(), &error);
    PT_ASSERT(absolute.empty(), "absolute path is rejected when root is configured");
    PT_ASSERT(error.find("absolute path") != std::string::npos, "absolute-path error is explicit");

    constexpr char nul_path_data[] =
        "safe.bin\0../pictor_unit_visus_resource_outside.bin";
    const std::string nul_path(nul_path_data, sizeof(nul_path_data) - 1);
    const std::vector<uint8_t> nul = loader.fetch(nul_path, &error);
    PT_ASSERT(nul.empty(), "embedded NUL path is rejected instead of opening a prefix");
    PT_ASSERT(error.find("NUL") != std::string::npos, "NUL-path error is explicit");

    fs::remove_all(root, ec);
    fs::remove(outside, ec);
    return report("unit_visus_resource_loader_test");
}
