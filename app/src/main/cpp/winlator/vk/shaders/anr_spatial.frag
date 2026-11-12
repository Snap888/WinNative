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
            vec3 tap = texelFetch(sourceTexture, clampPixel(base + ivec2(x-1, y-1), size), 0).rgb;
            sum += tap * wx[x] * wy[y];
        }
    }
    return sum;
}
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

void main() {
    ivec2 size = textureSize(sourceTexture, 0);
    vec2 texel = 1.0 / vec2(size);
    vec2 uv = clamp(vUV, texel * 0.5, vec2(1.0) - texel * 0.5);
    vec3 center = texture(sourceTexture, uv).rgb;
    vec3 bicubic = sampleCatmullRom(uv, size);
    float quality = clamp(pc.quality / 4.0, 0.0, 1.0);
    vec3 reconstructed = mix(center, bicubic, quality);

    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec3 n = texelFetch(sourceTexture, clampPixel(pixel + ivec2(0,-1), size), 0).rgb;
    vec3 s = texelFetch(sourceTexture, clampPixel(pixel + ivec2(0, 1), size), 0).rgb;
    vec3 w = texelFetch(sourceTexture, clampPixel(pixel + ivec2(-1,0), size), 0).rgb;
    vec3 e = texelFetch(sourceTexture, clampPixel(pixel + ivec2(1, 0), size), 0).rgb;
    float edge = clamp(max(abs(luma(e)-luma(w)), abs(luma(s)-luma(n))) * 3.0, 0.0, 1.0);
    vec3 detail = reconstructed - (n + s + w + e) * 0.25;
    float strength = clamp(pc.sharpness, 0.0, 2.0);
    vec3 result = reconstructed + detail * strength * 0.55 * mix(1.0, 0.62, edge);

    // Preserve real high-frequency detail; only limit extreme ringing.
    vec3 lo = min(center, min(min(n,s), min(w,e)));
    vec3 hi = max(center, max(max(n,s), max(w,e)));
    result = clamp(result, lo - 0.10, hi + 0.10);
    outColor = vec4(clamp(result, 0.0, 1.0), 1.0);
}