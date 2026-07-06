// "Disney-lite" uber-BSDF (PRD §3.3):
//   diffuse (Lambert) + GGX reflection (metal/dielectric) + GGX transmission.
// All directions are in the local shading frame (normal = +Z).
// eval() returns f and the *combined* sample pdf so NEE/MIS stays consistent
// with sample().
#ifndef BSDF_GLSL
#define BSDF_GLSL

#include "common.glsl"
#include "random.glsl"

struct Bsdf {
    vec3  baseColor;
    float roughness;
    float metallic;
    float transmission;
    float ior;       // relative to outside (air)
    bool  entering;  // geometric front face
};

struct BsdfSample {
    vec3  wi;        // local space
    vec3  f;         // BSDF value
    float pdf;
    bool  valid;
};

// ---- GGX helpers (alpha = roughness^2, isotropic) -------------------------
float ggxD(vec3 h, float alpha) {
    float a2 = alpha * alpha;
    float c2 = h.z * h.z;
    float d = c2 * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-12);
}

float ggxLambda(vec3 w, float alpha) {
    float c2 = w.z * w.z;
    float t2 = max(1.0 - c2, 0.0) / max(c2, 1e-12);
    return 0.5 * (sqrt(1.0 + alpha * alpha * t2) - 1.0);
}

float ggxG1(vec3 w, float alpha) { return 1.0 / (1.0 + ggxLambda(w, alpha)); }
float ggxG2(vec3 wo, vec3 wi, float alpha) {
    return 1.0 / (1.0 + ggxLambda(wo, alpha) + ggxLambda(wi, alpha));
}

// VNDF sampling (Heitz 2018). wo.z > 0 assumed.
vec3 sampleGgxVndf(vec3 wo, float alpha, vec2 u) {
    vec3 vh = normalize(vec3(alpha * wo.x, alpha * wo.y, wo.z));
    float lensq = vh.x * vh.x + vh.y * vh.y;
    vec3 T1 = lensq > 0.0 ? vec3(-vh.y, vh.x, 0.0) * inversesqrt(lensq) : vec3(1, 0, 0);
    vec3 T2 = cross(vh, T1);
    float r = sqrt(u.x);
    float phi = TWO_PI * u.y;
    float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    float s = 0.5 * (1.0 + vh.z);
    t2 = (1.0 - s) * sqrt(max(1.0 - t1 * t1, 0.0)) + s * t2;
    vec3 nh = t1 * T1 + t2 * T2 + sqrt(max(1.0 - t1 * t1 - t2 * t2, 0.0)) * vh;
    return normalize(vec3(alpha * nh.x, alpha * nh.y, max(nh.z, 1e-6)));
}

vec3 fresnelSchlick(vec3 f0, float cosTheta) {
    float m = clamp(1.0 - cosTheta, 0.0, 1.0);
    float m2 = m * m;
    return f0 + (1.0 - f0) * (m2 * m2 * m);
}

float fresnelDielectric(float cosI, float eta) { // eta = n_t / n_i
    cosI = clamp(cosI, 0.0, 1.0);
    float sin2T = (1.0 - cosI * cosI) / (eta * eta);
    if (sin2T >= 1.0) return 1.0; // TIR
    float cosT = sqrt(1.0 - sin2T);
    float rs = (cosI - eta * cosT) / (cosI + eta * cosT);
    float rp = (eta * cosI - cosT) / (eta * cosI + cosT);
    return 0.5 * (rs * rs + rp * rp);
}

vec2 sampleCosineHemispherePdf(vec2 u, out vec3 wi) { // returns (cos, pdf)
    float r = sqrt(u.x);
    float phi = TWO_PI * u.y;
    wi = vec3(r * cos(phi), r * sin(phi), sqrt(max(1.0 - u.x, 0.0)));
    return vec2(wi.z, wi.z * INV_PI);
}

// ---- lobe weights ----------------------------------------------------------
void lobeWeights(Bsdf m, vec3 wo, out float wDiff, out float wSpec, out float wTrans) {
    vec3 f0 = mix(vec3(0.04), m.baseColor, m.metallic);
    wSpec  = luminance(fresnelSchlick(f0, abs(wo.z)));
    wDiff  = (1.0 - m.metallic) * (1.0 - m.transmission) * luminance(m.baseColor);
    wTrans = (1.0 - m.metallic) * m.transmission;
    float total = wDiff + wSpec + wTrans;
    if (total < 1e-7) { wDiff = 1.0; wSpec = 0.0; wTrans = 0.0; total = 1.0; }
    wDiff /= total; wSpec /= total; wTrans /= total;
}

// ---- eval: f and pdf for a given (wo, wi) ----------------------------------
// wo.z > 0 by construction (frame flipped to the viewer side).
// wi.z > 0 -> reflection side, wi.z < 0 -> transmission side.
void bsdfEval(Bsdf m, vec3 wo, vec3 wi, out vec3 f, out float pdf) {
    f = vec3(0.0);
    pdf = 0.0;
    float alpha = max(m.roughness * m.roughness, 1e-3);
    float wDiff, wSpec, wTrans;
    lobeWeights(m, wo, wDiff, wSpec, wTrans);
    float eta = m.entering ? m.ior : 1.0 / m.ior; // n_t / n_i along wo->wi

    if (wi.z > 0.0) {
        // Diffuse
        if (wDiff > 0.0) {
            vec3 fd = (1.0 - m.metallic) * (1.0 - m.transmission)
                    * m.baseColor * INV_PI;
            f += fd * 1.0;
            pdf += wDiff * wi.z * INV_PI;
        }
        // Specular reflection
        vec3 h = normalize(wo + wi);
        float d = ggxD(h, alpha);
        float g2 = ggxG2(wo, wi, alpha);
        vec3 f0 = mix(vec3(0.04), m.baseColor, m.metallic);
        vec3 F = fresnelSchlick(f0, dot(wo, h));
        f += d * g2 * F / max(4.0 * wo.z * wi.z, 1e-7);
        pdf += wSpec * ggxG1(wo, alpha) * d / max(4.0 * wo.z, 1e-7);
    } else if (wTrans > 0.0) {
        // Microfacet transmission (Walter 2007), wi.z < 0.
        vec3 h = normalize(wo + wi * eta);
        if (h.z < 0.0) h = -h;
        float woH = dot(wo, h);
        float wiH = dot(wi, h);
        if (woH * wiH < 0.0) { // opposite hemispheres of h: valid refraction
            float F = fresnelDielectric(abs(woH), eta);
            float d = ggxD(h, alpha);
            float g2 = ggxG2(wo, vec3(wi.x, wi.y, -wi.z), alpha);
            float denom = woH + eta * wiH;
            denom = max(denom * denom, 1e-7);
            float jac = eta * eta * abs(wiH) / denom;
            f += m.baseColor * (1.0 - F) * d * g2
               * abs(woH) * jac / max(abs(wo.z * wi.z), 1e-7)
               * (1.0 - m.metallic) * m.transmission;
            pdf += wTrans * (1.0 - F) * ggxG1(wo, alpha) * d
                 * abs(woH) / max(wo.z, 1e-7) * jac;
        }
    }
}

// ---- sample ----------------------------------------------------------------
BsdfSample bsdfSample(Bsdf m, vec3 wo, inout Rng rng) {
    BsdfSample s;
    s.valid = false;

    float alpha = max(m.roughness * m.roughness, 1e-3);
    float wDiff, wSpec, wTrans;
    lobeWeights(m, wo, wDiff, wSpec, wTrans);
    float eta = m.entering ? m.ior : 1.0 / m.ior;

    float pick = rng1(rng);
    vec3 wi;

    if (pick < wDiff) {
        vec3 d;
        sampleCosineHemispherePdf(rng2(rng), d);
        wi = d;
    } else if (pick < wDiff + wSpec) {
        vec3 h = sampleGgxVndf(wo, alpha, rng2(rng));
        wi = reflect(-wo, h);
        if (wi.z <= 0.0) return s;
    } else {
        vec3 h = sampleGgxVndf(wo, alpha, rng2(rng));
        float woH = dot(wo, h);
        float F = fresnelDielectric(abs(woH), eta);
        if (rng1(rng) < F) {
            wi = reflect(-wo, h); // TIR / Fresnel reflection of the trans lobe
            if (wi.z <= 0.0) return s;
        } else {
            // refract with relative IOR eta (n_t/n_i): GLSL refract takes n_i/n_t.
            wi = refract(-wo, h, 1.0 / eta);
            if (wi.z >= 0.0 || length(wi) < 1e-6) return s;
            wi = normalize(wi);
        }
    }

    vec3 f;
    float pdf;
    bsdfEval(m, wo, wi, f, pdf);
    if (pdf < 1e-9) return s;

    s.wi = wi;
    s.f = f;
    s.pdf = pdf;
    s.valid = true;
    return s;
}

#endif // BSDF_GLSL
