#version 450

layout(set = 0, binding = 0) uniform sampler2D scene_tex;
layout(set = 0, binding = 1) uniform sampler2D bloom_tex;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform PostPC {
    vec2 texel;
    float bloom_strength;
    float bloom_radius_px;
} pc;

float luma(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

void main() {
    vec3 scene = texture(scene_tex, uv).rgb;
    vec3 scene_n = texture(scene_tex, uv + vec2(0.0, -pc.texel.y)).rgb;
    vec3 scene_s = texture(scene_tex, uv + vec2(0.0, pc.texel.y)).rgb;
    vec3 scene_e = texture(scene_tex, uv + vec2(pc.texel.x, 0.0)).rgb;
    vec3 scene_w = texture(scene_tex, uv + vec2(-pc.texel.x, 0.0)).rgb;
    vec3 scene_avg = (scene_n + scene_s + scene_e + scene_w) * 0.25;
    float edge = smoothstep(0.025, 0.16, abs(luma(scene) - luma(scene_avg)));
    vec3 scene_aa = mix(scene, scene_avg, edge * 0.6);

    vec3 bloom = texture(bloom_tex, uv).rgb;
    vec3 color = scene_aa + bloom;
    out_color = vec4(color, 1.0);
}
