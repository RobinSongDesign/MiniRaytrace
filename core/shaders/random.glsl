// Stateless PCG-based RNG (pcg4d, Jarzynski & Olano 2020).
// Seeded per (pixel, frame, salt); successive draws advance a local state.
#ifndef RANDOM_GLSL
#define RANDOM_GLSL

uvec4 pcg4d(uvec4 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.w; v.y += v.z * v.x; v.z += v.x * v.y; v.w += v.y * v.z;
    v ^= v >> 16u;
    v.x += v.y * v.w; v.y += v.z * v.x; v.z += v.x * v.y; v.w += v.y * v.z;
    return v;
}

struct Rng { uvec4 state; };

Rng rngInit(uvec2 pixel, uint frame) {
    Rng r;
    r.state = uvec4(pixel, frame, 0u);
    return r;
}

float rng1(inout Rng r) {
    r.state.w++;
    return float(pcg4d(r.state).x) * (1.0 / 4294967296.0);
}

vec2 rng2(inout Rng r) {
    r.state.w++;
    uvec4 v = pcg4d(r.state);
    return vec2(v.xy) * (1.0 / 4294967296.0);
}

#endif // RANDOM_GLSL
