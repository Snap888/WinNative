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
                   0.5*f.y + 2.0*f2.y - 1.5*f3.y,
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
    return sum / max(total, 1.0e-5);
}

// Adaptive mode needs both kernels to classify edges. Fetch each source texel once
// and accumulate Catmull-Rom and Lanczos2 together instead of issuing 32 fetches.
void sampleAdaptiveKernels(vec2 uv, ivec2 size, out vec3 cubic, out vec3 lanczos) {
    vec2 p = uv * vec2(size) - 0.5;
    ivec2 base = ivec2(floor(p));
    vec2 f = fract(p), f2 = f * f, f3 = f2 * f;
    vec4 cx = vec4(-0.5*f.x + f2.x - 0.5*f3.x,
                   1.0 - 2.5*f2.x + 1.5*f3.x,
                   0.5*f.x + 2.0*f2.x - 1.5*f3.x,
                   -0.5*f2.x + 0.5*f3.x);
    vec4 cy = vec4(-0.5*f.y + f2.y - 0.5*f3.y,
                   1.0 - 2.5*f2.y + 1.5*f3.y,
                   0.5*f.y + 2.0*f2.y - 1.5*f3.y,
                   -0.5*f2.y + 0.5*f3.y);
    vec4 lx, ly;
    for (int i = 0; i < 4; ++i) {
        float d = float(i - 1);
        lx[i] = lanczos2Weight(d - f.x);
        ly[i] = lanczos2Weight(d - f.y);
    }
    cubic = vec3(0.0);
    lanczos = vec3(0.0);
    float lanczosTotal = 0.0;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            vec3 tap = texelFetch(sourceTexture,
                clampPixel(base + ivec2(x - 1, y - 1), size), 0).rgb;
            float cw = cx[x] * cy[y];
            float lw = lx[x] * ly[y];
            cubic += tap * cw;
            lanczos += tap * lw;
            lanczosTotal += lw;
        }
    }
    lanczos /= max(lanczosTotal, 1.0e-5);
}

float luma(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

void main() {
    ivec2 size = textureSize(sourceTexture, 0);
    vec2 texel = 1.0 / vec2(size);
    vec2 uv = clamp(vUV, texel * 0.5, vec2(1.0) - texel * 0.5);
    vec3 center = texture(sourceTexture, uv).rgb;
    int quality = int(clamp(floor(pc.quality + 0.5), 0.0, 6.0));

    vec3 reconstructed = center;
    vec3 neighborN = vec3(0.0), neighborS = vec3(0.0);
    vec3 neighborW = vec3(0.0), neighborE = vec3(0.0);
    bool neighborsReady = false;
    if (quality == 1) {
        reconstructed = sampleCatmullRom(uv, size);
    } else if (quality == 2) {
        reconstructed = sampleLanczos2(uv, size);
    } else if (quality >= 3) {
        vec3 cubic, lanczos;
        sampleAdaptiveKernels(uv, size, cubic, lanczos);

        ivec2 pixel = clampPixel(ivec2(floor(uv * vec2(size))), size);
        neighborN = texelFetch(sourceTexture, clampPixel(pixel + ivec2(0, -1), size), 0).rgb;
        neighborS = texelFetch(sourceTexture, clampPixel(pixel + ivec2(0,  1), size), 0).rgb;
        neighborW = texelFetch(sourceTexture, clampPixel(pixel + ivec2(-1, 0), size), 0).rgb;
        neighborE = texelFetch(sourceTexture, clampPixel(pixel + ivec2( 1, 0), size), 0).rgb;
        neighborsReady = true;

        float gx = abs(luma(neighborE) - luma(neighborW));
        float gy = abs(luma(neighborS) - luma(neighborN));
        float edge = clamp(max(gx, gy) * 4.0, 0.0, 1.0);
        float anisotropy = abs(gx - gy) / max(gx + gy, 1.0e-4);

        // Prefer the sharper Lanczos kernel in flat areas; use the more stable
        // cubic kernel across strong, directional edges to limit ringing.
        float cubicWeight = clamp(edge * (0.55 + 0.35 * anisotropy), 0.0, 0.9);
        reconstructed = mix(lanczos, cubic, cubicWeight);

        // Anti-ringing clamp to the local 3x3 source neighborhood.
        vec3 lo = min(center, min(min(neighborN, neighborS), min(neighborW, neighborE)));
        vec3 hi = max(center, max(max(neighborN, neighborS), max(neighborW, neighborE)));
        reconstructed = clamp(reconstructed, lo - 0.035, hi + 0.035);

        if (quality == 4) {
            float localContrast = clamp((gx + gy) * 2.0, 0.0, 1.0);
            vec3 crossAverage = (neighborN + neighborS + neighborW + neighborE) * 0.25;
            vec3 detail = center - crossAverage;
            reconstructed += detail * (0.12 * localContrast);
            reconstructed = clamp(reconstructed, lo - 0.035, hi + 0.035);
        } else if (quality == 5) {
            // Detail Recovery+: recover fine texture along the local edge normal while
            // keeping the edge tangent stable. The luma-only correction avoids chroma halos.
            vec3 nw = texelFetch(sourceTexture, clampPixel(pixel + ivec2(-1, -1), size), 0).rgb;
            vec3 ne = texelFetch(sourceTexture, clampPixel(pixel + ivec2( 1, -1), size), 0).rgb;
            vec3 sw = texelFetch(sourceTexture, clampPixel(pixel + ivec2(-1,  1), size), 0).rgb;
            vec3 se = texelFetch(sourceTexture, clampPixel(pixel + ivec2( 1,  1), size), 0).rgb;
            float diagonalContrast = abs(luma(ne) - luma(sw)) + abs(luma(nw) - luma(se));
            float edgeStrength = clamp(max(gx, gy) * 3.0, 0.0, 1.0);
            vec3 normalAverage = gx >= gy ? (neighborW + neighborE) * 0.5
                                          : (neighborN + neighborS) * 0.5;
            vec3 diagonalAverage = (nw + ne + sw + se) * 0.25;
            float directionalDetail = luma(center) - luma(normalAverage);
            float fineDetail = luma(center) - luma(diagonalAverage);
            float recovered = mix(fineDetail, directionalDetail, edgeStrength);
            float textureGate = smoothstep(0.008, 0.12, max(gx + gy, diagonalContrast * 0.5));
            float correction = clamp(recovered, -0.055, 0.055) * textureGate * 0.22;
            reconstructed += vec3(correction);

            vec3 lo8 = min(lo, min(min(nw, ne), min(sw, se)));
            vec3 hi8 = max(hi, max(max(nw, ne), max(sw, se)));
            reconstructed = clamp(reconstructed, lo8 - 0.018, hi8 + 0.018);
        } else if (quality == 6) {
            // Fine Texture+: restrained high-frequency recovery with silhouette protection.
            vec3 nw = texelFetch(sourceTexture, clampPixel(pixel + ivec2(-1, -1), size), 0).rgb;
            vec3 ne = texelFetch(sourceTexture, clampPixel(pixel + ivec2( 1, -1), size), 0).rgb;
            vec3 sw = texelFetch(sourceTexture, clampPixel(pixel + ivec2(-1,  1), size), 0).rgb;
            vec3 se = texelFetch(sourceTexture, clampPixel(pixel + ivec2( 1,  1), size), 0).rgb;
            float neighborhood = (luma(neighborN) + luma(neighborS)
                                + luma(neighborW) + luma(neighborE)
                                + luma(nw) + luma(ne) + luma(sw) + luma(se)) * 0.125;
            float highFrequency = luma(center) - neighborhood;
            float localGradient = max(gx, gy);
            float textureGate = smoothstep(0.004, 0.032, abs(highFrequency));
            float edgeGate = 1.0 - 0.72 * smoothstep(0.075, 0.22, localGradient);
            float detail = clamp(highFrequency, -0.035, 0.035)
                         * 0.30 * textureGate * edgeGate;
            reconstructed += vec3(detail);
            vec3 lo8 = min(lo, min(min(nw, ne), min(sw, se)));
            vec3 hi8 = max(hi, max(max(nw, ne), max(sw, se)));
            reconstructed = clamp(reconstructed, lo8 - 0.012, hi8 + 0.012);
        }
    }

    // Sharpening is independent from reconstruction quality and operates on
    // luma only, avoiding colored halos around high-contrast UI edges.
    float strength = clamp(pc.sharpness, 0.0, 3.0);
    if (strength > 0.001) {
        ivec2 pixel = clampPixel(ivec2(floor(uv * vec2(size))), size);
        if (!neighborsReady) {
            neighborN = texelFetch(sourceTexture, clampPixel(pixel + ivec2(0, -1), size), 0).rgb;
            neighborS = texelFetch(sourceTexture, clampPixel(pixel + ivec2(0,  1), size), 0).rgb;
            neighborW = texelFetch(sourceTexture, clampPixel(pixel + ivec2(-1, 0), size), 0).rgb;
            neighborE = texelFetch(sourceTexture, clampPixel(pixel + ivec2( 1, 0), size), 0).rgb;
        }
        float edge = clamp(max(abs(luma(neighborE) - luma(neighborW)),
                               abs(luma(neighborS) - luma(neighborN))) * 3.0, 0.0, 1.0);
        float detail = luma(reconstructed)
                     - luma((neighborN + neighborS + neighborW + neighborE) * 0.25);
        float edgeProtection = quality == 5 ? 0.42 : 0.58;
        float sharpenedLuma = luma(reconstructed) + detail * strength * 0.42
                            * mix(1.0, edgeProtection, edge);
        float delta = sharpenedLuma - luma(reconstructed);
        reconstructed += vec3(delta);
    }

    outColor = vec4(clamp(reconstructed, 0.0, 1.0), 1.0);
}
