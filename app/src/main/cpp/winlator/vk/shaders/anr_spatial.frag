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

float luma(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

void main() {
    vec2 texel = 1.0 / vec2(textureSize(sourceTexture, 0));
    vec2 uv = clamp(vUV, texel * 0.5, vec2(1.0) - texel * 0.5);

    vec3 c  = texture(sourceTexture, uv).rgb;
    vec3 n  = texture(sourceTexture, uv + vec2(0.0, -texel.y)).rgb;
    vec3 s  = texture(sourceTexture, uv + vec2(0.0,  texel.y)).rgb;
    vec3 w  = texture(sourceTexture, uv + vec2(-texel.x, 0.0)).rgb;
    vec3 e  = texture(sourceTexture, uv + vec2( texel.x, 0.0)).rgb;
    vec3 nw = texture(sourceTexture, uv + vec2(-texel.x, -texel.y)).rgb;
    vec3 ne = texture(sourceTexture, uv + vec2( texel.x, -texel.y)).rgb;
    vec3 sw = texture(sourceTexture, uv + vec2(-texel.x,  texel.y)).rgb;
    vec3 se = texture(sourceTexture, uv + vec2( texel.x,  texel.y)).rgb;

    float gx = abs(luma(e) - luma(w)) + 0.5 * (abs(luma(ne) - luma(nw)) + abs(luma(se) - luma(sw)));
    float gy = abs(luma(s) - luma(n)) + 0.5 * (abs(luma(sw) - luma(nw)) + abs(luma(se) - luma(ne)));

    vec3 axisH = (c * 2.0 + n + s) * 0.25;
    vec3 axisV = (c * 2.0 + w + e) * 0.25;
    float edgeConfidence = clamp(abs(gx - gy) * 10.0, 0.0, 1.0);
    vec3 reconstructed = mix((axisH + axisV) * 0.5, gx < gy ? axisH : axisV, edgeConfidence);

    float edge = clamp(max(gx, gy) * 4.0, 0.0, 1.0);
    float qualityRadius = mix(0.65, 1.0, clamp(pc.quality / 3.0, 0.0, 1.0));
    float sharpen = clamp(pc.sharpness, 0.0, 1.0) * qualityRadius;
    vec3 neighbourhood = (n + s + w + e) * 0.25;
    vec3 result = reconstructed + (c - neighbourhood) * sharpen * (0.35 + edge * 0.65);

    vec3 lo = min(c, min(min(n, s), min(w, e)));
    vec3 hi = max(c, max(max(n, s), max(w, e)));
    result = clamp(result, lo - 0.02, hi + 0.02);
    outColor = vec4(clamp(result, 0.0, 1.0), 1.0);
}
