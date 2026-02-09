#version 450

layout(set = 0, binding = 0) uniform sampler2D scene_tex;
layout(set = 0, binding = 1) uniform sampler2D bloom_tex;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform PostPC {
    vec2 texel;
    float bloom_strength;
    float bloom_radius_px;
    float vignette_strength;
    float barrel_distortion;
    float scanline_strength;
    float noise_strength;
    float time_s;
} pc;

float luma(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

vec3 fxaa_scene(sampler2D tex, vec2 p, vec2 texel) {
    vec3 rgb_m  = texture(tex, p).rgb;
    vec3 rgb_nw = texture(tex, p + texel * vec2(-1.0, -1.0)).rgb;
    vec3 rgb_ne = texture(tex, p + texel * vec2( 1.0, -1.0)).rgb;
    vec3 rgb_sw = texture(tex, p + texel * vec2(-1.0,  1.0)).rgb;
    vec3 rgb_se = texture(tex, p + texel * vec2( 1.0,  1.0)).rgb;

    float l_m = luma(rgb_m);
    float l_nw = luma(rgb_nw);
    float l_ne = luma(rgb_ne);
    float l_sw = luma(rgb_sw);
    float l_se = luma(rgb_se);

    float l_min = min(l_m, min(min(l_nw, l_ne), min(l_sw, l_se)));
    float l_max = max(l_m, max(max(l_nw, l_ne), max(l_sw, l_se)));
    if (l_max - l_min < 0.015) {
        return rgb_m;
    }

    vec2 dir;
    dir.x = -((l_nw + l_ne) - (l_sw + l_se));
    dir.y =  ((l_nw + l_sw) - (l_ne + l_se));

    float dir_reduce = max((l_nw + l_ne + l_sw + l_se) * 0.125 * 0.25, 1.0 / 128.0);
    float rcp_dir_min = 1.0 / (min(abs(dir.x), abs(dir.y)) + dir_reduce);
    dir = clamp(dir * rcp_dir_min, vec2(-8.0), vec2(8.0)) * texel;

    vec3 rgb_a = 0.5 * (
        texture(tex, p + dir * (1.0 / 3.0 - 0.5)).rgb +
        texture(tex, p + dir * (2.0 / 3.0 - 0.5)).rgb
    );
    vec3 rgb_b = rgb_a * 0.5 + 0.25 * (
        texture(tex, p + dir * -0.5).rgb +
        texture(tex, p + dir * 0.5).rgb
    );

    float l_b = luma(rgb_b);
    if (l_b < l_min || l_b > l_max) {
        return rgb_a;
    }
    return rgb_b;
}

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

vec2 barrel_uv(vec2 p, float k) {
    vec2 q = p * 2.0 - 1.0;
    float r2 = dot(q, q);
    q *= 1.0 + k * r2;
    return q * 0.5 + 0.5;
}

void main() {
    vec2 ui_max = vec2(592.0 * pc.texel.x, 704.0 * pc.texel.y);
    bool in_ui = (uv.x <= ui_max.x && uv.y <= ui_max.y);

    float barrel_k = in_ui ? 0.0 : pc.barrel_distortion;
    vec2 uv_dist = barrel_uv(uv, barrel_k);
    vec2 uv_clamped = clamp(uv_dist, vec2(0.0), vec2(1.0));

    vec3 scene_aa = fxaa_scene(scene_tex, uv_clamped, pc.texel);
    vec3 bloom = texture(bloom_tex, uv_clamped).rgb;
    vec3 color = scene_aa + bloom * (in_ui ? 0.18 : 1.0);

    if (!in_ui) {
        float vig_r = length(uv - vec2(0.5)) * 1.4142;
        float vig = 1.0 - pc.vignette_strength * smoothstep(0.35, 1.0, vig_r);
        color *= max(vig, 0.0);

        float scan = 0.5 + 0.5 * sin((uv.y + pc.time_s * 0.015) * 3.14159265 * 720.0);
        color *= 1.0 - scan * pc.scanline_strength * 0.35;

        float n = hash12(uv * vec2(1920.0, 1080.0) + vec2(pc.time_s * 31.17, pc.time_s * 17.03)) - 0.5;
        color += n * pc.noise_strength * 0.12;
    }

    color = max(color, vec3(0.0));
    out_color = vec4(color, 1.0);
}
