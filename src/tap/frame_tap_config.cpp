#include "pictor/tap/frame_tap_config.h"

#include <cstdlib>

namespace pictor {

namespace {

bool equals_ignore_case(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

} // namespace

FrameTapSetting parse_frame_tap_setting(std::string_view value) {
    FrameTapSetting setting;
    if (value.empty() || value == "0" || equals_ignore_case(value, "off")) {
        return setting;
    }
    if (value == "-" || equals_ignore_case(value, "stdout")) {
        setting.output = FrameTapOutput::STDOUT;
        return setting;
    }
    setting.output = FrameTapOutput::FILE;
    setting.path   = std::string(value);
    return setting;
}

std::string read_frame_tap_env() {
#ifdef _MSC_VER
    // MSVC は std::getenv を C4996 で警告するため _dupenv_s を使う。
    char*  value = nullptr;
    size_t size  = 0;
    if (_dupenv_s(&value, &size, kFrameTapEnvVar) != 0 || value == nullptr) return {};
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(kFrameTapEnvVar);
    return value ? std::string(value) : std::string();
#endif
}

std::unique_ptr<IFrameTapSink> make_frame_tap_sink(const FrameTapSetting& setting,
                                                   std::string*           error) {
    switch (setting.output) {
        case FrameTapOutput::NONE:
            return nullptr;
        case FrameTapOutput::STDOUT:
            return std::make_unique<StdoutFrameTapSink>();
        case FrameTapOutput::FILE:
            return FileFrameTapSink::open(setting.path, error);
    }
    if (error) *error = "unknown frame tap output";
    return nullptr;
}

} // namespace pictor
