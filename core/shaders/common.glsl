// Shared declarations: bindings, buffer layouts, constants.
// Must stay byte-identical with core/src/render/GpuTypes.hpp.
#ifndef COMMON_GLSL
#define COMMON_GLSL

#define PI       3.14159265358979323846
#define TWO_PI   6.28318530717958647692
#define INV_PI   0.31830988618379067154
#define EPS_RAY  1e-4
#define MAX_TEXTURES 64

#define FLAG_HAS_ENV      1u
#define FLAG_USE_DENOISED 2u
#define FLAG_ORTHOGRAPHIC 4u

layout(std430, binding = 0) readonly buffer GlobalsBuf {
    vec4  camPos;
    vec4  camRight;     // unit right vector
    vec4  camUp;        // unit up vector
    vec4  camForward;   // unit forward vector
    vec4  camFrustum;   // left, right, bottom, top (dist=1 slice; absolute if FLAG_ORTHOGRAPHIC)
    uint  width; uint height; uint frameIndex; uint maxBounces;
    uint  lightCount; uint envCdfWidth; uint envCdfHeight; uint flags;
    float envRotation; float envIntensity; float envIntegral; float exposure;
    float fireflyClamp; uint tonemapMode; uint tlasRoot; uint debugView;
} g;

layout(std430, binding = 1) buffer AccumBuf     { vec4 accum[]; };
layout(std430, binding = 2) buffer AovAlbedoBuf { vec4 aovAlbedo[]; };
layout(std430, binding = 3) buffer AovNormalBuf { vec4 aovNormal[]; };
layout(std430, binding = 4) readonly buffer DenoisedBuf { vec4 denoised[]; };

// BVH node, 32 bytes. count == 0 -> internal (leftOrPrim = left child,
// right = left + 1). Leaf -> leftOrPrim = first primitive, count prims.
// packed = (count << 2) | splitAxis.
struct BvhNode { vec4 lo; vec4 hi; }; // lo.w/hi.w carry uint bits
layout(std430, binding = 5) readonly buffer NodesBuf { BvhNode nodes[]; };

layout(std430, binding = 6) readonly buffer TriBuf  { uvec4 triangles[]; }; // i0,i1,i2,pad
layout(std430, binding = 7) readonly buffer PosBuf  { vec4 positions[]; };
layout(std430, binding = 8) readonly buffer NrmBuf  { vec4 normals[]; };
layout(std430, binding = 9) readonly buffer UvBuf   { vec2 uvs[]; };
layout(std430, binding = 10) readonly buffer TanBuf { vec4 tangents[]; }; // w = handedness

struct Instance {
    mat4 objectToWorld;
    mat4 worldToObject;
    uint blasRoot; uint materialIndex; uint _p0; uint _p1;
};
layout(std430, binding = 11) readonly buffer InstBuf { Instance instances[]; };

struct Material {
    vec4  baseColorOpacity;   // rgb + opacity
    vec4  emissionRoughness;  // rgb + roughness
    vec4  params;             // metallic, transmission, ior, pad
    ivec4 tex0;               // baseColor, roughness, metallic, normal
    ivec4 tex1;               // emission, pad, pad, pad
};
layout(std430, binding = 12) readonly buffer MatBuf { Material materials[]; };

#define LIGHT_ENV   0
#define LIGHT_SUN   1
#define LIGHT_POINT 2
#define LIGHT_RECT  3
struct Light {
    vec4  p0; // sun: dir+cosAngRad | point: pos | rect: corner+area
    vec4  p1; // sun: radiance | point: intensity | rect: edge0
    vec4  p2; // rect: edge1
    vec4  p3; // rect: radiance + twoSided
    ivec4 meta; // x = type
};
layout(std430, binding = 13) readonly buffer LightBuf { Light lights[]; };

// [conditional h*w][marginal h], both normalized row CDFs.
layout(std430, binding = 14) readonly buffer EnvCdfBuf { float envCdf[]; };

layout(binding = 15) uniform sampler2D envMap;
layout(binding = 16) uniform sampler2D textures[MAX_TEXTURES];

struct Ray { vec3 origin; vec3 dir; float tMax; };

struct HitInfo {
    float t;
    uint  triIndex;      // global triangle index
    uint  instanceIndex;
    vec2  bary;          // barycentric (b1, b2)
    bool  hit;
};

float luminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
float maxComp(vec3 v)   { return max(v.x, max(v.y, v.z)); }

// Branchless ONB (Duff et al. 2017).
void buildOnb(vec3 n, out vec3 t, out vec3 b) {
    float s = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float c = n.x * n.y * a;
    t = vec3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = vec3(c, s + n.y * n.y * a, -n.y);
}

#endif // COMMON_GLSL
