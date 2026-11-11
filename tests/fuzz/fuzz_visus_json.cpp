// Fuzz entry: the Visus JSON reader (from_visus_json: hand-written JSON parser,
// v1 compatibility, metadata values).
#include "fuzz_entry.h"

#include "pictor/visus/visus_serializer.h"

#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string json(reinterpret_cast<const char*>(data), size);
    pictor::VisusDesc desc;
    std::string error;
    std::vector<std::string> warnings;
    (void)pictor::from_visus_json(json, desc, &error, &warnings);
    return 0;
}

std::vector<std::vector<uint8_t>> pictor_fuzz_seeds() {
    const std::string seeds[] = {
        R"({"version":2,"name":"cube","metadata":{"tags":["a","b"],"n":1.5,"ok":true,"z":null}})",
        R"({"name":"legacy","mesh":"cube.obj","material":{"albedo":[1,0.5,0.25,1]}})",
        R"({"version":2,"name":"x","children":[{"name":"child"}]})",
    };
    std::vector<std::vector<uint8_t>> out;
    for (const auto& s : seeds) out.emplace_back(s.begin(), s.end());
    return out;
}
