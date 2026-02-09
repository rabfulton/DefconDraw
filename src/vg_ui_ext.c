#include "vg_ui_ext.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static float vg_ui_ext_clampf(float v, float lo, float hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static float vg_ui_ext_norm(float v, float lo, float hi) {
    if (hi <= lo) {
        return 0.0f;
    }
    return vg_ui_ext_clampf((v - lo) / (hi - lo), 0.0f, 1.0f);
}

static vg_fill_style vg_ui_ext_fill_from_stroke(const vg_stroke_style* s, float alpha_scale) {
    vg_fill_style f;
    f.intensity = s->intensity;
    f.color = s->color;
    f.color.a *= alpha_scale;
    f.blend = s->blend;
    return f;
}

static vg_result vg_ui_ext_draw_arc(
    vg_context* ctx,
    vg_vec2 center,
    float radius,
    float a0,
    float a1,
    int steps,
    const vg_stroke_style* style
) {
    if (steps < 2) {
        steps = 2;
    }
    vg_vec2* pts = (vg_vec2*)malloc(sizeof(*pts) * (size_t)steps);
    if (!pts) {
        return VG_ERROR_OUT_OF_MEMORY;
    }
    for (int i = 0; i < steps; ++i) {
        float t = (float)i / (float)(steps - 1);
        float a = a0 + (a1 - a0) * t;
        pts[i].x = center.x + cosf(a) * radius;
        pts[i].y = center.y + sinf(a) * radius;
    }
    vg_result r = vg_draw_polyline(ctx, pts, (size_t)steps, style, 0);
    free(pts);
    return r;
}

vg_result vg_ui_meter_linear(vg_context* ctx, const vg_ui_meter_desc* desc, const vg_ui_meter_style* style) {
    if (!ctx || !desc || !style) {
        return VG_ERROR_INVALID_ARGUMENT;
    }
    if (desc->rect.w <= 0.0f || desc->rect.h <= 0.0f) {
        return VG_ERROR_INVALID_ARGUMENT;
    }

    float value01 = vg_ui_ext_norm(desc->value, desc->min_value, desc->max_value);
    vg_result r = vg_draw_rect(ctx, desc->rect, &style->frame);
    if (r != VG_OK) {
        return r;
    }

    float pad = style->frame.width_px + 2.0f;
    if (pad > desc->rect.w * 0.35f) {
        pad = desc->rect.w * 0.35f;
    }
    if (pad > desc->rect.h * 0.35f) {
        pad = desc->rect.h * 0.35f;
    }
    vg_rect inner = {
        desc->rect.x + pad,
        desc->rect.y + pad,
        desc->rect.w - 2.0f * pad,
        desc->rect.h - 2.0f * pad
    };
    if (inner.w <= 1.0f || inner.h <= 1.0f) {
        return VG_OK;
    }

    vg_fill_style bg_fill = vg_ui_ext_fill_from_stroke(&style->bg, 0.45f);
    r = vg_fill_rect(ctx, inner, &bg_fill);
    if (r != VG_OK) {
        return r;
    }

    vg_fill_style fg_fill = vg_ui_ext_fill_from_stroke(&style->fill, 0.75f);
    if (desc->mode == VG_UI_METER_SEGMENTED) {
        int segs = desc->segments > 0 ? desc->segments : 10;
        float gap = desc->segment_gap_px >= 0.0f ? desc->segment_gap_px : 2.0f;
        float seg_w = (inner.w - (float)(segs - 1) * gap) / (float)segs;
        if (seg_w < 1.0f) {
            seg_w = 1.0f;
            gap = (inner.w - (float)segs * seg_w) / (float)(segs - 1 > 0 ? segs - 1 : 1);
            if (gap < 0.0f) {
                gap = 0.0f;
            }
        }
        int lit = (int)floorf(value01 * (float)segs + 1e-5f);
        if (lit < 0) lit = 0;
        if (lit > segs) lit = segs;
        for (int i = 0; i < lit; ++i) {
            vg_rect seg = {inner.x + (seg_w + gap) * (float)i, inner.y, seg_w, inner.h};
            r = vg_fill_rect(ctx, seg, &fg_fill);
            if (r != VG_OK) {
                return r;
            }
        }
    } else {
        vg_rect fill = inner;
        fill.w = inner.w * value01;
        if (fill.w > 0.5f) {
            r = vg_fill_rect(ctx, fill, &fg_fill);
            if (r != VG_OK) {
                return r;
            }
        }
    }

    if (desc->show_ticks) {
        const int nt = 5;
        for (int i = 0; i <= nt; ++i) {
            float u = (float)i / (float)nt;
            float x = inner.x + inner.w * u;
            vg_vec2 tick[2] = {
                {x, inner.y},
                {x, inner.y + inner.h * 0.24f}
            };
            r = vg_draw_polyline(ctx, tick, 2u, &style->tick, 0);
            if (r != VG_OK) {
                return r;
            }
        }
    }

    if (desc->label && desc->label[0] != '\0') {
        r = vg_draw_text(
            ctx,
            desc->label,
            (vg_vec2){desc->rect.x, desc->rect.y + desc->rect.h + 8.0f},
            12.0f,
            0.9f,
            &style->text,
            NULL
        );
        if (r != VG_OK) {
            return r;
        }
    }

    if (desc->show_value) {
        char vtxt[64];
        const char* fmt = (desc->value_fmt && desc->value_fmt[0] != '\0') ? desc->value_fmt : "%.1f";
        snprintf(vtxt, sizeof(vtxt), fmt, desc->value);
        float tw = vg_measure_text(vtxt, 12.0f, 0.8f);
        r = vg_draw_text(
            ctx,
            vtxt,
            (vg_vec2){desc->rect.x + desc->rect.w - tw, desc->rect.y + desc->rect.h + 8.0f},
            12.0f,
            0.8f,
            &style->text,
            NULL
        );
        if (r != VG_OK) {
            return r;
        }
    }

    return VG_OK;
}

vg_result vg_ui_meter_radial(vg_context* ctx, vg_vec2 center, float radius_px, const vg_ui_meter_desc* desc, const vg_ui_meter_style* style) {
    if (!ctx || !desc || !style || !isfinite(radius_px) || radius_px <= 1.0f) {
        return VG_ERROR_INVALID_ARGUMENT;
    }

    float value01 = vg_ui_ext_norm(desc->value, desc->min_value, desc->max_value);
    const float a0 = 3.926990716f;  /* 225 deg */
    const float sweep = 4.712388980f; /* 270 deg */
    const float a1 = a0 + sweep;
    vg_result r = vg_ui_ext_draw_arc(ctx, center, radius_px, a0, a1, 72, &style->bg);
    if (r != VG_OK) {
        return r;
    }

    if (desc->mode == VG_UI_METER_SEGMENTED) {
        int segs = desc->segments > 0 ? desc->segments : 18;
        float gap_px = desc->segment_gap_px >= 0.0f ? desc->segment_gap_px : 3.0f;
        float gap_a = gap_px / radius_px;
        float seg_a = (sweep - gap_a * (float)(segs - 1)) / (float)segs;
        if (seg_a < 0.02f) {
            seg_a = 0.02f;
            gap_a = 0.0f;
        }
        int lit = (int)floorf(value01 * (float)segs + 1e-5f);
        if (lit < 0) lit = 0;
        if (lit > segs) lit = segs;
        for (int i = 0; i < lit; ++i) {
            float s0 = a0 + (seg_a + gap_a) * (float)i;
            float s1 = s0 + seg_a;
            r = vg_ui_ext_draw_arc(ctx, center, radius_px, s0, s1, 10, &style->fill);
            if (r != VG_OK) {
                return r;
            }
        }
    } else {
        r = vg_ui_ext_draw_arc(ctx, center, radius_px, a0, a0 + sweep * value01, 72, &style->fill);
        if (r != VG_OK) {
            return r;
        }
    }

    r = vg_ui_ext_draw_arc(ctx, center, radius_px + style->frame.width_px * 0.6f, a0, a1, 72, &style->frame);
    if (r != VG_OK) {
        return r;
    }

    if (desc->show_ticks) {
        for (int i = 0; i <= 10; ++i) {
            float u = (float)i / 10.0f;
            float a = a0 + sweep * u;
            float c = cosf(a);
            float s = sinf(a);
            vg_vec2 tick[2] = {
                {center.x + c * (radius_px - 6.0f), center.y + s * (radius_px - 6.0f)},
                {center.x + c * (radius_px + 4.0f), center.y + s * (radius_px + 4.0f)}
            };
            r = vg_draw_polyline(ctx, tick, 2u, &style->tick, 0);
            if (r != VG_OK) {
                return r;
            }
        }
    }

    /* Needle */
    {
        float an = a0 + sweep * value01;
        vg_vec2 needle[2] = {
            center,
            {center.x + cosf(an) * (radius_px - 8.0f), center.y + sinf(an) * (radius_px - 8.0f)}
        };
        r = vg_draw_polyline(ctx, needle, 2u, &style->tick, 0);
        if (r != VG_OK) {
            return r;
        }
    }

    if (desc->show_value) {
        char vtxt[64];
        const char* fmt = (desc->value_fmt && desc->value_fmt[0] != '\0') ? desc->value_fmt : "%.1f";
        snprintf(vtxt, sizeof(vtxt), fmt, desc->value);
        float tw = vg_measure_text(vtxt, 12.0f, 0.8f);
        r = vg_draw_text(ctx, vtxt, (vg_vec2){center.x - tw * 0.5f, center.y - 6.0f}, 12.0f, 0.8f, &style->text, NULL);
        if (r != VG_OK) {
            return r;
        }
    }

    if (desc->label && desc->label[0] != '\0') {
        float tw = vg_measure_text(desc->label, 11.0f, 0.8f);
        r = vg_draw_text(ctx, desc->label, (vg_vec2){center.x - tw * 0.5f, center.y - radius_px - 18.0f}, 11.0f, 0.8f, &style->text, NULL);
        if (r != VG_OK) {
            return r;
        }
    }

    return VG_OK;
}
