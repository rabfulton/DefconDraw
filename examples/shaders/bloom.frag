#version 450

layout(set = 0, binding = 0) uniform sampler2D scene_tex;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform PostPC {
    vec2 texel;
    float bloom_strength;
    float bloom_radius_px;
} pc;

void main() {
    vec3 c = vec3(0.0);
    float w = 0.0;

    const float weights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);

    c += texture(scene_tex, uv).rgb * weights[0];
    w += weights[0];

    float radius = max(pc.bloom_radius_px, 0.25);
    for (int i = 1; i < 5; ++i) {
        vec2 d = pc.texel * float(i) * radius;
        c += texture(scene_tex, uv + vec2(d.x, 0.0)).rgb * weights[i];
        c += texture(scene_tex, uv - vec2(d.x, 0.0)).rgb * weights[i];
        c += texture(scene_tex, uv + vec2(0.0, d.y)).rgb * weights[i];
        c += texture(scene_tex, uv - vec2(0.0, d.y)).rgb * weights[i];
        w += 4.0 * weights[i];
    }

    c /= max(w, 1e-5);
    out_color = vec4(c * pc.bloom_strength, 1.0);
}
