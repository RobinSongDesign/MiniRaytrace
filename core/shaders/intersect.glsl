// Two-level BVH traversal: TLAS over instances, BLAS over triangles.
// Object-space rays keep the *unnormalized* transformed direction so the
// t parameter is shared between spaces (PRD §4).
#ifndef INTERSECT_GLSL
#define INTERSECT_GLSL

#include "common.glsl"

// Slab test; returns entry distance or tMax when missed.
float intersectAabb(vec3 lo, vec3 hi, vec3 origin, vec3 invDir, float tMax) {
    vec3 t0 = (lo - origin) * invDir;
    vec3 t1 = (hi - origin) * invDir;
    vec3 tminv = min(t0, t1);
    vec3 tmaxv = max(t0, t1);
    float tEnter = max(max(tminv.x, tminv.y), max(tminv.z, 0.0));
    float tExit  = min(min(tmaxv.x, tmaxv.y), min(tmaxv.z, tMax));
    return tEnter <= tExit ? tEnter : tMax;
}

// Moeller-Trumbore. Returns t > 0 on hit (writes bary), or -1.
float intersectTriangle(uint triIndex, vec3 origin, vec3 dir, float tMax, out vec2 bary) {
    uvec4 tri = triangles[triIndex];
    vec3 v0 = positions[tri.x].xyz;
    vec3 e1 = positions[tri.y].xyz - v0;
    vec3 e2 = positions[tri.z].xyz - v0;

    vec3 p = cross(dir, e2);
    float det = dot(e1, p);
    if (abs(det) < 1e-12) return -1.0;
    float invDet = 1.0 / det;

    vec3 s = origin - v0;
    float u = dot(s, p) * invDet;
    if (u < 0.0 || u > 1.0) return -1.0;

    vec3 q = cross(s, e1);
    float v = dot(dir, q) * invDet;
    if (v < 0.0 || u + v > 1.0) return -1.0;

    float t = dot(e2, q) * invDet;
    if (t <= EPS_RAY || t >= tMax) return -1.0;

    bary = vec2(u, v);
    return t;
}

// Traverse one BLAS in object space. Updates hit/tMax (t is world == object t).
void traverseBlas(uint rootNode, uint instanceIdx, vec3 origin, vec3 dir,
                  inout float tMax, inout HitInfo hit) {
    vec3 invDir = 1.0 / dir;
    uint stack[32];
    int sp = 0;
    stack[sp++] = rootNode;

    while (sp > 0) {
        BvhNode node = nodes[stack[--sp]];
        if (intersectAabb(node.lo.xyz, node.hi.xyz, origin, invDir, tMax) >= tMax)
            continue;

        uint leftOrPrim = floatBitsToUint(node.lo.w);
        uint packed     = floatBitsToUint(node.hi.w);
        uint count      = packed >> 2;

        if (count == 0u) {
            // Near child first based on ray direction along the split axis.
            uint axis = packed & 3u;
            uint near = leftOrPrim, far = leftOrPrim + 1u;
            if (dir[axis] < 0.0) { near = far; far = leftOrPrim; }
            stack[sp++] = far;
            stack[sp++] = near;
        } else {
            for (uint i = 0u; i < count; ++i) {
                vec2 bary;
                float t = intersectTriangle(leftOrPrim + i, origin, dir, tMax, bary);
                if (t > 0.0) {
                    tMax = t;
                    hit.t = t;
                    hit.triIndex = leftOrPrim + i;
                    hit.instanceIndex = instanceIdx;
                    hit.bary = bary;
                    hit.hit = true;
                }
            }
        }
    }
}

HitInfo traceRay(Ray ray) {
    HitInfo hit;
    hit.hit = false;
    hit.t = ray.tMax;

    float tMax = ray.tMax;
    vec3 invDir = 1.0 / ray.dir;

    uint stack[16];
    int sp = 0;
    stack[sp++] = g.tlasRoot;

    while (sp > 0) {
        BvhNode node = nodes[stack[--sp]];
        if (intersectAabb(node.lo.xyz, node.hi.xyz, ray.origin, invDir, tMax) >= tMax)
            continue;

        uint leftOrPrim = floatBitsToUint(node.lo.w);
        uint packed     = floatBitsToUint(node.hi.w);
        uint count      = packed >> 2;

        if (count == 0u) {
            uint axis = packed & 3u;
            uint near = leftOrPrim, far = leftOrPrim + 1u;
            if (ray.dir[axis] < 0.0) { near = far; far = leftOrPrim; }
            stack[sp++] = far;
            stack[sp++] = near;
        } else {
            // TLAS leaf: one instance. Transform the ray and descend.
            uint instIdx = leftOrPrim;
            Instance inst = instances[instIdx];
            vec3 oObj = (inst.worldToObject * vec4(ray.origin, 1.0)).xyz;
            vec3 dObj = (inst.worldToObject * vec4(ray.dir, 0.0)).xyz;
            traverseBlas(inst.blasRoot, instIdx, oObj, dObj, tMax, hit);
        }
    }
    return hit;
}

// Shadow ray: any hit terminates (opaque shadows in v1).
bool traceShadowRay(Ray ray) {
    float tMax = ray.tMax;
    vec3 invDir = 1.0 / ray.dir;

    uint stack[16];
    int sp = 0;
    stack[sp++] = g.tlasRoot;

    while (sp > 0) {
        BvhNode node = nodes[stack[--sp]];
        if (intersectAabb(node.lo.xyz, node.hi.xyz, ray.origin, invDir, tMax) >= tMax)
            continue;

        uint leftOrPrim = floatBitsToUint(node.lo.w);
        uint packed     = floatBitsToUint(node.hi.w);
        uint count      = packed >> 2;

        if (count == 0u) {
            stack[sp++] = leftOrPrim;
            stack[sp++] = leftOrPrim + 1u;
        } else {
            Instance inst = instances[leftOrPrim];
            vec3 oObj = (inst.worldToObject * vec4(ray.origin, 1.0)).xyz;
            vec3 dObj = (inst.worldToObject * vec4(ray.dir, 0.0)).xyz;
            vec3 invObj = 1.0 / dObj;

            uint bstack[32];
            int bsp = 0;
            bstack[bsp++] = inst.blasRoot;
            while (bsp > 0) {
                BvhNode bn = nodes[bstack[--bsp]];
                if (intersectAabb(bn.lo.xyz, bn.hi.xyz, oObj, invObj, tMax) >= tMax)
                    continue;
                uint blop = floatBitsToUint(bn.lo.w);
                uint bpk  = floatBitsToUint(bn.hi.w);
                uint bcnt = bpk >> 2;
                if (bcnt == 0u) {
                    bstack[bsp++] = blop;
                    bstack[bsp++] = blop + 1u;
                } else {
                    for (uint i = 0u; i < bcnt; ++i) {
                        vec2 bary;
                        if (intersectTriangle(blop + i, oObj, dObj, tMax, bary) > 0.0)
                            return true;
                    }
                }
            }
        }
    }
    return false;
}

#endif // INTERSECT_GLSL
