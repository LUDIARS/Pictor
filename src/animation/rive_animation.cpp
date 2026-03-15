#include "pictor/animation/rive_animation.h"
#include <fstream>
#include <cstring>
#include <algorithm>

namespace pictor {

/// Internal implementation (placeholder for Rive runtime integration)
struct RiveAnimation::Impl {
    // Rive runtime objects would go here when linking against rive-cpp
    // For now, we store parsed artboard data for the API surface
    std::string current_artboard;
    std::string current_animation;
    std::string current_state_machine;
    float       animation_time = 0.0f;
    float       animation_duration = 1.0f;

    std::vector<RiveArtboardDescriptor> artboards;
    std::vector<RiveInput> inputs;
};

RiveAnimation::RiveAnimation() = default;
RiveAnimation::~RiveAnimation() = default;

bool RiveAnimation::load_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        error_ = "Failed to open Rive file: " + path;
        return false;
    }

    auto size = file.tellg();
    file.seekg(0, std::ios::beg);

    file_data_.resize(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(file_data_.data()), size)) {
        error_ = "Failed to read Rive file: " + path;
        return false;
    }

    return load_memory(file_data_.data(), file_data_.size());
}

bool RiveAnimation::load_memory(const uint8_t* data, size_t size) {
    if (!data || size < 4) {
        error_ = "Invalid Rive data";
        return false;
    }

    // Rive files start with "RIVE" magic (Flare2d format)
    // or have a specific binary header for .riv format
    impl_ = std::make_unique<Impl>();

    // Parse basic header to extract artboard info
    // The actual Rive runtime would handle full parsing
    file_data_.assign(data, data + size);

    // Create a default artboard descriptor
    RiveArtboardDescriptor default_ab;
    default_ab.name = "Default";
    default_ab.width = 512.0f;
    default_ab.height = 512.0f;
    default_ab.animation_names = {"idle"};
    default_ab.state_machine_names = {"State Machine 1"};
    impl_->artboards.push_back(default_ab);

    artboard_width_ = default_ab.width;
    artboard_height_ = default_ab.height;
    loaded_ = true;
    error_.clear();

    return true;
}

bool RiveAnimation::select_artboard(const std::string& name) {
    if (!impl_) return false;

    if (name.empty() && !impl_->artboards.empty()) {
        impl_->current_artboard = impl_->artboards[0].name;
        artboard_width_ = impl_->artboards[0].width;
        artboard_height_ = impl_->artboards[0].height;
        return true;
    }

    for (const auto& ab : impl_->artboards) {
        if (ab.name == name) {
            impl_->current_artboard = ab.name;
            artboard_width_ = ab.width;
            artboard_height_ = ab.height;
            return true;
        }
    }

    error_ = "Artboard not found: " + name;
    return false;
}

RiveArtboardDescriptor RiveAnimation::get_artboard_info() const {
    if (!impl_ || impl_->artboards.empty()) return {};

    for (const auto& ab : impl_->artboards) {
        if (ab.name == impl_->current_artboard) return ab;
    }
    return impl_->artboards[0];
}

bool RiveAnimation::play_animation(const std::string& name) {
    if (!impl_) return false;
    impl_->current_animation = name;
    impl_->animation_time = 0.0f;
    animation_playing_ = true;
    return true;
}

void RiveAnimation::stop_animation() {
    if (!impl_) return;
    animation_playing_ = false;
    impl_->animation_time = 0.0f;
}

bool RiveAnimation::activate_state_machine(const std::string& name) {
    if (!impl_) return false;
    impl_->current_state_machine = name;
    return true;
}

void RiveAnimation::set_input_bool(const std::string& name, bool value) {
    if (!impl_) return;
    for (auto& input : impl_->inputs) {
        if (input.name == name && input.type == RiveInputType::BOOLEAN) {
            input.boolean_value = value;
            return;
        }
    }
    impl_->inputs.push_back({name, RiveInputType::BOOLEAN, 0.0f, value});
}

void RiveAnimation::set_input_number(const std::string& name, float value) {
    if (!impl_) return;
    for (auto& input : impl_->inputs) {
        if (input.name == name && input.type == RiveInputType::NUMBER) {
            input.number_value = value;
            return;
        }
    }
    impl_->inputs.push_back({name, RiveInputType::NUMBER, value, false});
}

void RiveAnimation::fire_input_trigger(const std::string& name) {
    if (!impl_) return;
    // Triggers are fire-and-forget; the state machine processes them
    // In a real implementation, this would push to the state machine
    (void)name;
}

void RiveAnimation::advance(float delta_time) {
    if (!impl_ || !animation_playing_) return;

    impl_->animation_time += delta_time;
    if (impl_->animation_time >= impl_->animation_duration) {
        impl_->animation_time = std::fmod(impl_->animation_time, impl_->animation_duration);
    }
}

void RiveAnimation::render_to_buffer(uint8_t* buffer, uint32_t width, uint32_t height) {
    if (!buffer || width == 0 || height == 0) return;

    // Clear buffer to transparent
    std::memset(buffer, 0, width * height * 4);

    // In a full implementation, this would use the Rive renderer
    // to rasterize the current artboard state into the pixel buffer.
    // The Rive C++ runtime (rive-cpp) provides a Renderer interface
    // that can target software buffers or GPU backends.
}

} // namespace pictor
