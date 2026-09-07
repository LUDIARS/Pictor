#pragma once
#include <string>

namespace pictor_kuzuha {
struct FrameRecording {std::string directory;unsigned count=300,fps=30;};
bool parse_recording_option(int argc,char** argv,int& index,FrameRecording& recording);
std::string recording_path(const FrameRecording& recording,unsigned index);
void prepare_recording(const FrameRecording& recording);
}
