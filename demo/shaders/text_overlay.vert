#version 450

layout(push_constant) uniform PushConstants {
    float screen_width;
    float screen_height;
};

layout(location = 0) in vec2 inPos;    // pixel coordinates (top-left origin)
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec4 fragColor;

void main() {
    // Pixel coords -> NDC (Vulkan: Y down = top-left origin matches)
    float x = (inPos.x / screen_width)  * 2.0 - 1.0;
    float y = (inPos.y / screen_height) * 2.0 - 1.0;
    gl_Position = vec4(x, y, 0.0, 1.0);
    fragUV = inUV;
    fragColor = inColor;
}
