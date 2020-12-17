#version 460 core
#extension GL_GOOGLE_include_directive : require

#include "cubes.inc"

layout (std140, set = 0, binding = 0) uniform ubo_uniforms {
    uniform_data uniforms;
};

layout (rgba16f, set = 0, binding = 1) restrict readonly uniform image2D img_output;

layout (location = 0) in vec2 in_uv;
layout (location = 0) out vec4 out_color;

void main() {
    ivec2 coord = ivec2(in_uv * vec2(uniforms.viewport.zw));
	vec4 frag_color = imageLoad(img_output, coord);
	out_color = frag_color;
}
