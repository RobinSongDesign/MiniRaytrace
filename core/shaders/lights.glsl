// Light sampling: HDRI importance sampling + analytic lights, NEE with MIS.
// The environment, when present, is light index 0 (set up by the CPU packer)
// so pick probability is identical on both MIS strategies.
#ifndef LIGHTS_GLSL
#define LIGHTS_GLSL

#include "common.glsl"
#include "random.glsl"

// ---- environment map -------------------------------------------------------
vec2 dirToEquirect(vec3 d) {
    float phi = atan(d.z, d.x) - g.envRotation;
    float theta = acos(clamp(d.y, -1.0, 1.0));
    return vec2(fract(phi / TWO_PI + 1.0), theta / PI);
}

vec3 equirectToDir(vec2 uv) {
    float phi = uv.x * TWO_PI + g.envRotation;
    float theta = uv.y * PI;
    float st = sin(theta);
    return vec3(st * cos(phi), cos(theta), st * sin(phi));
}

vec3 envRadiance(vec3 dir) {
    if ((g.flags & FLAG_HAS_ENV) == 0u) return vec3(0.0);
    return texture(envMap, dirToEquirect(dir)).rgb * g.envIntensity;
}

// Solid-angle pdf of the CDF sampler: lum(dir) / ∫ lum dω  (see Renderer.cpp).
float envPdf(vec3 dir) {
    if ((g.flags & FLAG_HAS_ENV) == 0u) return 0.0;
    vec3 c = textureLod(envMap, dirToEquirect(dir), 0.0).rgb;
    return luminance(c) / g.envIntegral;
}

// Binary search a normalized CDF in envCdf[base .. base+count).
uint searchCdf(uint base, uint count, float u) {
    uint lo = 0u, hi = count - 1u;
    while (lo < hi) {
        uint mid = (lo + hi) >> 1u;
        if (envCdf[base + mid] < u) lo = mid + 1u;
        else hi = mid;
    }
    return lo;
}

// Sample a direction from the environment; returns radiance, dir, pdf.
bool sampleEnv(inout Rng rng, out vec3 dir, out vec3 radiance, out float pdf) {
    uint w = g.envCdfWidth, h = g.envCdfHeight;
    if (w == 0u || h == 0u) return false;
    uint row = searchCdf(w * h, h, rng1(rng));            // marginal
    uint col = searchCdf(row * w, w, rng1(rng));          // conditional
    vec2 uv = vec2((float(col) + 0.5) / float(w), (float(row) + 0.5) / float(h));
    dir = equirectToDir(uv);
    radiance = textureLod(envMap, uv, 0.0).rgb * g.envIntensity;
    pdf = envPdf(dir);
    return pdf > 1e-9;
}

// ---- NEE: sample one light uniformly ---------------------------------------
struct LightSample {
    vec3  wiWorld;     // direction to the light
    vec3  radiance;    // incident radiance (or I/d^2 for point lights)
    float pdf;         // solid-angle pdf * pick probability (0 for delta)
    float dist;        // shadow ray length
    bool  delta;       // true: skip MIS (point/sun treated as delta-ish)
    bool  valid;
};

LightSample sampleOneLight(vec3 p, inout Rng rng) {
    LightSample ls;
    ls.valid = false;
    if (g.lightCount == 0u) return ls;

    uint idx = min(uint(rng1(rng) * float(g.lightCount)), g.lightCount - 1u);
    float pickPdf = 1.0 / float(g.lightCount);
    Light l = lights[idx];

    if (l.meta.x == LIGHT_ENV) {
        vec3 dir, rad;
        float pdf;
        if (!sampleEnv(rng, dir, rad, pdf)) return ls;
        ls.wiWorld = dir;
        ls.radiance = rad;
        ls.pdf = pdf * pickPdf;
        ls.dist = 1e30;
        ls.delta = false;       // env participates in MIS
        ls.valid = true;
    } else if (l.meta.x == LIGHT_SUN) {
        // Uniform cone around the sun direction.
        float cosMax = l.p0.w;
        vec2 u = rng2(rng);
        float cosT = mix(1.0, cosMax, u.x);
        float sinT = sqrt(max(1.0 - cosT * cosT, 0.0));
        float phi = TWO_PI * u.y;
        vec3 t, b;
        buildOnb(l.p0.xyz, t, b);
        ls.wiWorld = normalize(t * (sinT * cos(phi)) + b * (sinT * sin(phi)) + l.p0.xyz * cosT);
        ls.radiance = l.p1.xyz;
        // Tiny cone: treated as delta for MIS, but the estimator still divides
        // by the cone solid-angle pdf (times pick probability).
        float conePdf = 1.0 / (TWO_PI * max(1.0 - cosMax, 1e-7));
        ls.pdf = conePdf * pickPdf;
        ls.dist = 1e30;
        ls.delta = true;
        ls.valid = true;
    } else if (l.meta.x == LIGHT_POINT) {
        vec3 d = l.p0.xyz - p;
        float d2 = max(dot(d, d), 1e-8);
        ls.dist = sqrt(d2);
        ls.wiWorld = d / ls.dist;
        ls.radiance = l.p1.xyz / d2;   // I / d^2
        ls.pdf = pickPdf;              // delta light: no solid-angle density
        ls.delta = true;
        ls.valid = true;
    } else { // LIGHT_RECT
        vec2 u = rng2(rng);
        vec3 q = l.p0.xyz + l.p1.xyz * u.x + l.p2.xyz * u.y;
        vec3 d = q - p;
        float d2 = max(dot(d, d), 1e-8);
        ls.dist = sqrt(d2);
        ls.wiWorld = d / ls.dist;
        vec3 n = normalize(cross(l.p1.xyz, l.p2.xyz));
        float cosL = dot(n, -ls.wiWorld);
        if (l.p3.w > 0.5) cosL = abs(cosL); // two-sided
        if (cosL < 1e-6) return ls;
        float area = max(l.p0.w, 1e-8);
        ls.radiance = l.p3.xyz;
        ls.pdf = (d2 / (cosL * area)) * pickPdf; // area pdf -> solid angle
        ls.delta = true; // rect lights are not hittable by BSDF rays: no MIS
        ls.valid = true;
    }
    return ls;
}

// Pick probability used for MIS on BSDF-sampled env hits.
float envPickPdf() {
    return ((g.flags & FLAG_HAS_ENV) != 0u && g.lightCount > 0u)
        ? 1.0 / float(g.lightCount) : 0.0;
}

#endif // LIGHTS_GLSL
