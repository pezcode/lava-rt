#ifndef CUBES_INC_HEADER_GUARD
#define CUBES_INC_HEADER_GUARD

struct uniform_data {
    mat4 inv_view;
    mat4 inv_proj;
    uvec4 viewport;
    vec3 background_color;
};

#ifdef HIT_SHADER

struct instance {
    uint vertex_base;
    uint vertex_count;
    uint index_base;
    uint index_count;
};

struct vertex {
    vec3 position;
    vec4 color;
    vec2 uv;
    vec3 normal;
};

struct triangle {
    vertex v0;
    vertex v1;
    vertex v2;
};

vec2 bary_lerp(vec2 a, vec2 b, vec2 c, vec3 barycentrics) {
    return a * barycentrics.x + b * barycentrics.y + c * barycentrics.z;
}

vec3 bary_lerp(vec3 a, vec3 b, vec3 c, vec3 barycentrics) {
    return a * barycentrics.x + b * barycentrics.y + c * barycentrics.z;
}

vec4 bary_lerp(vec4 a, vec4 b, vec4 c, vec3 barycentrics) {
    return a * barycentrics.x + b * barycentrics.y + c * barycentrics.z;
}

// get interpolated vertex attributes from hit coord barycentrics
vertex get_vertex(triangle tri, vec2 bary_coord) {
    vec3 barycentrics = vec3(1.0f - bary_coord.x - bary_coord.y, bary_coord.x, bary_coord.y);
    vertex v;

    //v.position = gl_WorldRayOriginEXT + (gl_HitTEXT * gl_WorldRayDirectionEXT);

    vec3 position = bary_lerp(tri.v0.position, tri.v1.position, tri.v2.position, barycentrics);
    v.position = gl_ObjectToWorldEXT * vec4(position, 1.0);

    v.color = bary_lerp(tri.v0.color, tri.v1.color, tri.v2.color, barycentrics);

    v.uv = bary_lerp(tri.v0.uv, tri.v1.uv, tri.v2.uv, barycentrics);

    vec3 normal = bary_lerp(tri.v0.normal, tri.v1.normal, tri.v2.normal, barycentrics);
    // TODO use correct normal matrix
    v.normal = gl_ObjectToWorldEXT * vec4(normal, 0.0);

    return v;
}

#endif // HIT_SHADER

struct ray_payload {
    vec3 color;
};

#endif // CUBES_INC_HEADER_GUARD
