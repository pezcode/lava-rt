#version 460 core
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_scalar_block_layout : enable

#define HIT_SHADER
#include "cubes.inc"

// hit attribute of the built-in triangle intersection shader
// barycentric weights of the point of intersection of ray with triangle
hitAttributeEXT vec2 barycentric_coord;

layout (set = 1, binding = 0) uniform accelerationStructureEXT top_level_as;

layout (scalar, set = 1, binding = 1) restrict readonly buffer sso_instances {
    // per mesh/instance data
    // index with gl_InstanceID/gl_InstanceCustomIndexEXT
    instance instances[];
};

// the scalar layout is needed to use the tightly-packed vertex buffer
// std430 would force us to duplicate the vertex buffer with different member alignment
layout (scalar, set = 1, binding = 2) restrict readonly buffer sso_vertices {
    vertex vertices[];
};

layout (scalar, set = 1, binding = 3) restrict readonly buffer sso_indices {
    // all indices
    // index with index_base + (gl_PrimitiveID * 3)
    uint indices[];
};

// output of this shader
layout (location = 0) rayPayloadInEXT ray_payload payload;

// input/output of the callable shader
layout(location = 1) callableDataEXT callable_payload lighting_payload;

triangle get_triangle(instance ins, uint primitive) {
    uint index_offset = ins.index_base + (primitive * 3);

    uint i0 = indices[index_offset + 0];
    uint i1 = indices[index_offset + 1];
    uint i2 = indices[index_offset + 2];

    triangle tri;

    tri.v0 = vertices[ins.vertex_base + i0];
    tri.v1 = vertices[ins.vertex_base + i1];
    tri.v2 = vertices[ins.vertex_base + i2];

    return tri;
}

void main() {
    instance ins = instances[gl_InstanceID];
    triangle tri = get_triangle(ins, gl_PrimitiveID);
    vertex v = get_vertex(tri, barycentric_coord);

    // we could calculate lighting in this closest-hit shader
    // this is just for demonstration purposes
    lighting_payload.color = v.color;
    lighting_payload.normal = v.normal;
    executeCallableEXT(
        0, // SBT callable index
        1 // payload location
        );

    payload.color = lighting_payload.color;
    payload.position = v.position + 0.0001 * v.normal;
    payload.direction = reflect(gl_WorldRayDirectionEXT, v.normal);
}
