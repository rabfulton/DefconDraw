#ifndef VG_UI_EXT_H
#define VG_UI_EXT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "vg.h"

typedef enum vg_ui_meter_mode {
    VG_UI_METER_CONTINUOUS = 0,
    VG_UI_METER_SEGMENTED = 1
} vg_ui_meter_mode;

typedef struct vg_ui_meter_style {
    vg_stroke_style frame;
    vg_stroke_style fill;
    vg_stroke_style bg;
    vg_stroke_style tick;
    vg_stroke_style text;
} vg_ui_meter_style;

typedef struct vg_ui_meter_desc {
    vg_rect rect;
    float min_value;
    float max_value;
    float value;
    vg_ui_meter_mode mode;
    int segments;
    float segment_gap_px;
    const char* label;
    const char* value_fmt;
    int show_value;
    int show_ticks;
} vg_ui_meter_desc;

vg_result vg_ui_meter_linear(vg_context* ctx, const vg_ui_meter_desc* desc, const vg_ui_meter_style* style);
vg_result vg_ui_meter_radial(vg_context* ctx, vg_vec2 center, float radius_px, const vg_ui_meter_desc* desc, const vg_ui_meter_style* style);

#ifdef __cplusplus
}
#endif

#endif
