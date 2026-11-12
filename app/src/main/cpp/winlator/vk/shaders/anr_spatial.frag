#version 450
precision highp float;
precision highp int;

layout(location = 0) in highp vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D sourceTexture;

layout(push_constant) uniform PC {
    vec2 resolution;
    float renderScale;
    float sharpness;
    float quality;
    float mode;
} pc;

ivec2 clampPixel(ivec2 p, ivec2 size) {
    return clamp(p, ivec2(0), size - ivec2(1));
}

float sinc(float x) {
    if (abs(x) < 1.0e-4) return 1.0;
    float pix = 3.141592653589793 * x;
    return sin(pix) / pix;
}

float lanczos2Weight(float x) {
    x = abs(x);
    return x < 2.0 ? sinc(x) * sinc(x * 0.5) : 0.0;
}

vec3 sampleCatmullRom(vec2 uv, ivec2 size) {
    vec2 p = uv * vec2(size) - 0.5;
    ivec2 base = ivec2(floor(p));
    vec2 f = fract(p), f2 = f * f, f3 = f2 * f;
    vec4 wx = vec4(-0.5*f.x + f2.x - 0.5*f3.x,
                   1.0 - 2.5*f2.x + 1.5*f3.x,
                   0.5*f.x + 2.0*f2.x - 1.5*f3.x,
                   -0.5*f2.x + 0.5*f3.x);
    vec4 wy = vec4(-0.5*f.y + f2.y - 0.5*f3.y,
                   1.0 - 2.5*f2.y + 1.5*f3.y,
                   0.5*f.y + 2.0*f.y*f.y - 1.5*f3.y,
                   -0.5*f2.y + 0.5*f3.y);
    vec3 sum = vec3(0.0);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            vec3 tap = texelFetch(sourceTexture,
                clampPixel(base + ivec2(x - 1, y - 1), size), 0).rgb;
            sum += tap * wx[x] * wy[y];
        }
    }
    return sum;
}

vec3 sampleLanczos2(vec2 uv, ivec2 size) {
    vec2 p = uv * vec2(size) - 0.5;
    ivec2 base = ivec2(floor(p));
    vec2 f = fract(p);
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int y = -1; y <= 2; ++y) {
        float wy = lanczos2Weight(float(y) - f.y);
        for (int x = -1; x <= 2; ++x) {
            float weight = wy * lanczos2Weight(float(x) - f.x);
            vec3 tap = texelFetch(sourceTexture,
                clampPixel(base + ivec2(x, y), size), 0).rgb;
            sum += tap * weight;
            total += weight;
        }
    }
    return sum / max(abs(total), 1.0e-5) * sign(total);
}

float luma(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

void main() {
    ivec2 size = textureSize(sourceTexture, 0);
    vec2 texel = 1.0 / vec2(size);
    vec2 uv = clamp(vUV, texel * 0.5, vec2(1.0) - texel * 0.5);
    vec3 center = texture(sourceTexture, uv).rgb;
    int quality = int(clamp(floor(pc.quality + 0.5), 0.0, 4.0));

    vec3 reconstructed = center;
    if (quality == 1) {
        reconstructed = sampleCatmullRom(uv, size);
    } else if (quality == 2) {
        reconstructed = sampleLanczos2(uv, size);
    } else if (quality >= 3) {
        vec3 cubic = sampleCatmullRom(uv, size);
        vec3 lanczos = sampleLanczos2(uv, size);

        ivec2 pixel = clampPixel(ivec2(floor(uv * vec2(size))), size);
        vec3 n = texelFetch(sourceTexture, clampPixel(pixel + ivec2(0, -1), size), 0).rgb;
        vec3 s = texelFetch(sourceTexture, clampPixel(pixel + ivec2(0,  1), size), 0).rgb;
        vec3 w = texelFetch(sourceTexture, clampPixel(pixel + ivec2(-1, 0), size), 0).rgb;
        vec3 e = texelFetch(sourceTexture, clampPixel(pixel + ivec2( 1, 0), size), 0).rgb;

        float gx = abs(luma(e) - luma(w));
        float gy = abs(luma(s) - luma(n));
        float edge = clamp(max(gx, gy) * 4.0, 0.0, 1.0);
        float anisotropy = abs(gx - gy) / max(gx + gy, 1.0e-4);

        // Prefer the sharper Lanczos kernel in flat areas; use the more stable
        // cubic kernel across strong, directional edges to limit ringing.
        float cubicWeight = clamp(edge * (0.55 + 0.35 * anisotropy), 0.0, 0.9);
        reconstructed = mix(lanczos, cubic, cubicWeight);

        // Anti-ringing clamp to the local 3x3 source neighborhood.
        vec3 lo = min(center, min(min(n, s), min(w, e)));
        vec3 hi = max(center, max(max(n, s), max(w, e)));
        reconstructed = clamp(reconstructed, lo - 0.035, hi + 0.035);

        if (quality == 4) {
            float localContrast = clamp((gx + gy) * 2.0, 0.0, 1.0);
            vec3 crossAverage = (n + s + w + e) * 0.25;
            vec3 detail = center - crossAverage;
            reconstructed += detail * (0.12 * localContrast);
            reconstructed = clamp(reconstructed, lo - 0.035, hi + 0.035);
        }
    }

    // Sharpening is independent from reconstruction quality and operates on
    // luma only, avoiding colored halos around high-contrast UI edges.
    float strength = clamp(pc.sharpness, 0.0, 2.0);
    if (strength > 0.001) {
        ivec2 pixel = clampPixel(ivec2(floor(uv * vec2(size))), size);
        vec3 n = texelFetch(sourceTexture, clampPixel(pixel + ivec2(0, -1), size), 0).rgb;
        vec3 s = texelFetch(sourceTexture, clampPixel(pixel + ivec2(0,  1), size), 0).rgb;
        vec3 w = texelFetch(sourceTexture, clampPixel(pixel + ivec2(-1, 0), size), 0).rgb;
        vec3 e = texelFetch(sourceTexture, clampPixel(pixel + ivec2( 1, 0), size), 0).rgb;
        float edge = clamp(max(abs(luma(e) - luma(w)),
                               abs(luma(s) - luma(n))) * 3.0, 0.0, 1.0);
        float detail = luma(reconstructed) - luma((n + s + w + e) * 0.25);
        float sharpenedLuma = luma(reconstructed) + detail * strength * 0.42
                            * mix(1.0, 0.58, edge);
        float delta = sharpenedLuma - luma(reconstructed);
        reconstructed += vec3(delta);
    }

    outColor = vec4(clamp(reconstructed, 0.0, 1.0), 1.0);
}
