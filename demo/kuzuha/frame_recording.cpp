#include "frame_recording.h"
#include <cstdio>
#include <filesystem>
#include <stdexcept>

namespace pictor_kuzuha {
/// @implements SPEC-PC-POLYNOMIAL-MOTION
bool parse_recording_option(int argc,char** argv,int& index,FrameRecording& recording) {
    const std::string arg=argv[index];
    if(arg!="--record-frames" && arg!="--record-count" && arg!="--record-fps")return false;
    if(++index>=argc)throw std::invalid_argument(arg+" requires a value");
    if(arg=="--record-frames") {recording.directory=argv[index];return true;}
    const std::string input=argv[index];size_t end=0;const auto number=std::stoul(input,&end);
    const unsigned maximum=arg=="--record-count"?1800:60;
    if(end!=input.size() || number<1 || number>maximum)throw std::invalid_argument("Invalid frame recording range");
    (arg=="--record-count"?recording.count:recording.fps)=static_cast<unsigned>(number);return true;
}
/// @implements SPEC-PC-POLYNOMIAL-MOTION
std::string recording_path(const FrameRecording& recording,unsigned index) {
    char file[32];std::snprintf(file,sizeof(file),"frame-%05u.bmp",index);
    return (std::filesystem::path(recording.directory)/file).string();
}
/// @implements SPEC-PC-POLYNOMIAL-MOTION
void prepare_recording(const FrameRecording& recording) {
    if(std::filesystem::exists(recording.directory) && !std::filesystem::is_empty(recording.directory))
        throw std::runtime_error("Recording directory must be new or empty");
    std::filesystem::create_directories(recording.directory);
}
}
