#include "pictor/animation/vector_animation.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <sstream>

namespace pictor {

void VectorAnimationPlayer::load(const VectorAnimationDescriptor& desc) {
    name_ = desc.name;
    width_ = desc.width;
    height_ = desc.height;
    duration_ = desc.duration;
    wrap_mode_ = desc.wrap_mode;
    layers_ = desc.layers;
    current_time_ = 0.0f;
}

bool VectorAnimationPlayer::load_svg(const std::string& svg_data) {
    if (svg_data.empty()) return false;

    // Parse SVG dimensions
    auto find_attr = [&](const std::string& attr) -> float {
        auto pos = svg_data.find(attr + "=\"");
        if (pos == std::string::npos) return 0.0f;
        pos += attr.size() + 2;
        try { return std::stof(svg_data.substr(pos)); }
        catch (...) { return 0.0f; }
    };

    width_ = find_attr("width");
    height_ = find_attr("height");
    if (width_ <= 0.0f) width_ = 512.0f;
    if (height_ <= 0.0f) height_ = 512.0f;

    // Parse viewBox if width/height not set
    auto vb_pos = svg_data.find("viewBox=\"");
    if (vb_pos != std::string::npos) {
        vb_pos += 9;
        auto vb_end = svg_data.find('"', vb_pos);
        if (vb_end != std::string::npos) {
            std::string vb = svg_data.substr(vb_pos, vb_end - vb_pos);
            std::istringstream vbs(vb);
            float vx, vy, vw, vh;
            if (vbs >> vx >> vy >> vw >> vh) {
                if (width_ <= 0.0f) width_ = vw;
                if (height_ <= 0.0f) height_ = vh;
            }
        }
    }

    // Parse SMIL animation duration
    auto dur_pos = svg_data.find("dur=\"");
    if (dur_pos != std::string::npos) {
        dur_pos += 5;
        try {
            duration_ = std::stof(svg_data.substr(dur_pos));
            // Handle "s" suffix
        } catch (...) {
            duration_ = 1.0f;
        }
    } else {
        duration_ = 1.0f;
    }

    // Parse SVG path data elements
    VectorLayer layer;
    layer.name = "svg_root";

    size_t search_pos = 0;
    while (true) {
        auto path_pos = svg_data.find("<path", search_pos);
        if (path_pos == std::string::npos) break;

        auto d_pos = svg_data.find("d=\"", path_pos);
        if (d_pos == std::string::npos) break;
        d_pos += 3;
        auto d_end = svg_data.find('"', d_pos);
        if (d_end == std::string::npos) break;

        std::string path_data = svg_data.substr(d_pos, d_end - d_pos);

        // Parse SVG path commands into our format
        VectorPath vpath;
        std::istringstream ps(path_data);
        char cmd;
        while (ps >> cmd) {
            PathCommand pc;
            switch (cmd) {
                case 'M': case 'm':
                    pc.type = PathCommandType::MOVE_TO;
                    ps >> pc.params[0] >> pc.params[1];
                    break;
                case 'L': case 'l':
                    pc.type = PathCommandType::LINE_TO;
                    ps >> pc.params[0] >> pc.params[1];
                    break;
                case 'Q': case 'q':
                    pc.type = PathCommandType::QUAD_TO;
                    ps >> pc.params[0] >> pc.params[1] >> pc.params[2] >> pc.params[3];
                    break;
                case 'C': case 'c':
                    pc.type = PathCommandType::CUBIC_TO;
                    ps >> pc.params[0] >> pc.params[1] >> pc.params[2]
                       >> pc.params[3] >> pc.params[4] >> pc.params[5];
                    break;
                case 'Z': case 'z':
                    pc.type = PathCommandType::CLOSE;
                    break;
                default:
                    continue;
            }
            vpath.commands.push_back(pc);
        }

        // Parse fill/stroke attributes
        auto parse_color = [&](const std::string& attr, float out[4]) {
            auto attr_pos = svg_data.find(attr + "=\"", path_pos);
            if (attr_pos != std::string::npos && attr_pos < d_end + 100) {
                attr_pos += attr.size() + 2;
                auto attr_end = svg_data.find('"', attr_pos);
                if (attr_end != std::string::npos) {
                    std::string color = svg_data.substr(attr_pos, attr_end - attr_pos);
                    if (color.size() == 7 && color[0] == '#') {
                        unsigned int r, g, b;
                        if (sscanf(color.c_str(), "#%02x%02x%02x", &r, &g, &b) == 3) {
                            out[0] = r / 255.0f;
                            out[1] = g / 255.0f;
                            out[2] = b / 255.0f;
                            out[3] = 1.0f;
                        }
                    }
                }
            }
        };

        parse_color("fill", vpath.fill_color);
        parse_color("stroke", vpath.stroke_color);

        VectorKeyframe kf;
        kf.time = 0.0f;
        kf.paths.push_back(vpath);
        kf.transform = Transform2D::identity();

        if (layer.keyframes.empty()) {
            layer.keyframes.push_back(kf);
        } else {
            layer.keyframes[0].paths.push_back(vpath);
        }

        search_pos = d_end + 1;
    }

    if (!layer.keyframes.empty()) {
        layers_.push_back(layer);
    }

    name_ = "svg_animation";
    return true;
}

float VectorAnimationPlayer::wrap_time(float time) const {
    if (duration_ <= 0.0f) return 0.0f;

    switch (wrap_mode_) {
        case WrapMode::ONCE:
            return (time >= duration_) ? duration_ : time;
        case WrapMode::LOOP: {
            float t = std::fmod(time, duration_);
            return (t < 0.0f) ? t + duration_ : t;
        }
        case WrapMode::PING_PONG: {
            float t = std::fmod(time, duration_ * 2.0f);
            if (t < 0.0f) t += duration_ * 2.0f;
            return (t > duration_) ? (duration_ * 2.0f - t) : t;
        }
        case WrapMode::CLAMP:
            return std::clamp(time, 0.0f, duration_);
    }
    return time;
}

void VectorAnimationPlayer::advance(float delta_time) {
    if (!playing_) return;
    current_time_ += delta_time * speed_;
}

VectorFrame VectorAnimationPlayer::evaluate() const {
    return evaluate_at(current_time_);
}

VectorFrame VectorAnimationPlayer::evaluate_at(float time) const {
    float wrapped = const_cast<VectorAnimationPlayer*>(this)->wrap_time(time);

    VectorFrame frame;
    frame.time = wrapped;

    for (const auto& layer : layers_) {
        if (!layer.visible || layer.keyframes.empty()) continue;

        if (layer.keyframes.size() == 1) {
            // Single keyframe — return as-is
            for (const auto& path : layer.keyframes[0].paths) {
                frame.paths.push_back(path);
            }
            frame.transforms.push_back(layer.keyframes[0].transform);
            continue;
        }

        // Find keyframe pair for this layer
        uint32_t lo = 0;
        uint32_t hi = static_cast<uint32_t>(layer.keyframes.size() - 1);
        for (uint32_t i = 0; i < layer.keyframes.size() - 1; ++i) {
            if (layer.keyframes[i].time <= wrapped && layer.keyframes[i + 1].time > wrapped) {
                lo = i;
                hi = i + 1;
                break;
            }
        }
        if (wrapped >= layer.keyframes.back().time) {
            lo = hi = static_cast<uint32_t>(layer.keyframes.size() - 1);
        }

        float span = layer.keyframes[hi].time - layer.keyframes[lo].time;
        float t = (span > 0.0f) ? (wrapped - layer.keyframes[lo].time) / span : 0.0f;

        VectorKeyframe interpolated = interpolate_keyframes(
            layer.keyframes[lo], layer.keyframes[hi], t);

        for (const auto& path : interpolated.paths) {
            frame.paths.push_back(path);
        }
        frame.transforms.push_back(interpolated.transform);
    }

    return frame;
}

VectorKeyframe VectorAnimationPlayer::interpolate_keyframes(const VectorKeyframe& a,
                                                             const VectorKeyframe& b,
                                                             float t) const {
    VectorKeyframe result;
    result.time = a.time + t * (b.time - a.time);

    // Interpolate transforms
    result.transform.x = a.transform.x + t * (b.transform.x - a.transform.x);
    result.transform.y = a.transform.y + t * (b.transform.y - a.transform.y);
    result.transform.rotation = a.transform.rotation + t * (b.transform.rotation - a.transform.rotation);
    result.transform.scale_x = a.transform.scale_x + t * (b.transform.scale_x - a.transform.scale_x);
    result.transform.scale_y = a.transform.scale_y + t * (b.transform.scale_y - a.transform.scale_y);
    result.transform.opacity = a.transform.opacity + t * (b.transform.opacity - a.transform.opacity);

    // Interpolate paths (morphing)
    size_t path_count = std::min(a.paths.size(), b.paths.size());
    result.paths.resize(path_count);

    for (size_t i = 0; i < path_count; ++i) {
        result.paths[i] = interpolate_paths(a.paths[i], b.paths[i], t);
    }

    // If one has more paths, append them directly
    if (a.paths.size() > path_count) {
        for (size_t i = path_count; i < a.paths.size(); ++i) {
            result.paths.push_back(a.paths[i]);
        }
    }

    return result;
}

VectorPath VectorAnimationPlayer::interpolate_paths(const VectorPath& a,
                                                     const VectorPath& b,
                                                     float t) const {
    VectorPath result;

    // Interpolate colors
    for (int i = 0; i < 4; ++i) {
        result.fill_color[i] = a.fill_color[i] + t * (b.fill_color[i] - a.fill_color[i]);
        result.stroke_color[i] = a.stroke_color[i] + t * (b.stroke_color[i] - a.stroke_color[i]);
    }
    result.stroke_width = a.stroke_width + t * (b.stroke_width - a.stroke_width);

    // Interpolate path commands (only if same count and types match)
    size_t cmd_count = std::min(a.commands.size(), b.commands.size());
    result.commands.resize(cmd_count);

    for (size_t i = 0; i < cmd_count; ++i) {
        result.commands[i].type = a.commands[i].type;
        if (a.commands[i].type == b.commands[i].type) {
            for (int p = 0; p < 7; ++p) {
                result.commands[i].params[p] =
                    a.commands[i].params[p] + t * (b.commands[i].params[p] - a.commands[i].params[p]);
            }
        } else {
            result.commands[i] = a.commands[i]; // Type mismatch: use source
        }
    }

    return result;
}

void VectorAnimationPlayer::rasterize(uint8_t* buffer, uint32_t width, uint32_t height) const {
    if (!buffer || width == 0 || height == 0) return;

    // Clear buffer
    std::memset(buffer, 0, width * height * 4);

    VectorFrame frame = evaluate();

    // Software rasterization of vector paths
    // This provides a basic scanline-based rasterizer for vector graphics.
    // For production use, integrate with a full vector renderer (NanoVG, Skia, etc.)

    float scale_x = static_cast<float>(width) / width_;
    float scale_y = static_cast<float>(height) / height_;

    for (const auto& path : frame.paths) {
        if (path.commands.empty()) continue;

        // For each path, collect vertices and rasterize fill
        std::vector<std::pair<float, float>> vertices;
        float cx = 0, cy = 0;

        for (const auto& cmd : path.commands) {
            switch (cmd.type) {
                case PathCommandType::MOVE_TO:
                    cx = cmd.params[0] * scale_x;
                    cy = cmd.params[1] * scale_y;
                    vertices.push_back({cx, cy});
                    break;
                case PathCommandType::LINE_TO:
                    cx = cmd.params[0] * scale_x;
                    cy = cmd.params[1] * scale_y;
                    vertices.push_back({cx, cy});
                    break;
                case PathCommandType::CLOSE:
                    if (!vertices.empty()) {
                        vertices.push_back(vertices[0]);
                    }
                    break;
                default:
                    // Approximate curves with line segments
                    cx = cmd.params[cmd.type == PathCommandType::QUAD_TO ? 2 : 4] * scale_x;
                    cy = cmd.params[cmd.type == PathCommandType::QUAD_TO ? 3 : 5] * scale_y;
                    vertices.push_back({cx, cy});
                    break;
            }
        }

        // Simple scanline fill using even-odd rule
        if (vertices.size() >= 3) {
            float min_y = vertices[0].second, max_y = vertices[0].second;
            for (const auto& v : vertices) {
                min_y = std::min(min_y, v.second);
                max_y = std::max(max_y, v.second);
            }

            int iy_start = std::max(0, static_cast<int>(min_y));
            int iy_end = std::min(static_cast<int>(height) - 1, static_cast<int>(max_y));

            uint8_t r = static_cast<uint8_t>(path.fill_color[0] * 255);
            uint8_t g = static_cast<uint8_t>(path.fill_color[1] * 255);
            uint8_t b = static_cast<uint8_t>(path.fill_color[2] * 255);
            uint8_t a = static_cast<uint8_t>(path.fill_color[3] * 255);

            for (int y = iy_start; y <= iy_end; ++y) {
                std::vector<float> intersections;
                float fy = static_cast<float>(y) + 0.5f;

                for (size_t i = 0; i + 1 < vertices.size(); ++i) {
                    float y0 = vertices[i].second, y1 = vertices[i + 1].second;
                    if ((y0 <= fy && y1 > fy) || (y1 <= fy && y0 > fy)) {
                        float t_val = (fy - y0) / (y1 - y0);
                        float ix = vertices[i].first + t_val * (vertices[i + 1].first - vertices[i].first);
                        intersections.push_back(ix);
                    }
                }

                std::sort(intersections.begin(), intersections.end());

                for (size_t i = 0; i + 1 < intersections.size(); i += 2) {
                    int x_start = std::max(0, static_cast<int>(intersections[i]));
                    int x_end = std::min(static_cast<int>(width) - 1, static_cast<int>(intersections[i + 1]));
                    for (int x = x_start; x <= x_end; ++x) {
                        uint32_t idx = (y * width + x) * 4;
                        buffer[idx + 0] = r;
                        buffer[idx + 1] = g;
                        buffer[idx + 2] = b;
                        buffer[idx + 3] = a;
                    }
                }
            }
        }
    }
}

} // namespace pictor
