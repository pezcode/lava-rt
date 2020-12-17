#version 460 core
#extension GL_GOOGLE_include_directive : require
#extension GL_NV_ray_tracing : require

#include "cubes.inc"

layout (location = 0) rayPayloadInNV ray_payload payload;

layout (std140, set = 0, binding = 0) uniform ubo_uniforms {
    uniform_data uniforms;
};

void main() {
    payload.color = uniforms.background_color;
}
