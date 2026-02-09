# API Reference

This document describes the current public API in `include/vg.h`.

## Versioning

- `VG_VERSION_MAJOR`, `VG_VERSION_MINOR`, `VG_VERSION_PATCH`
- Current version in tree: `0.1.0`
- The API is pre-1.0 and may change.

## Core Concepts

- `vg_context`: top-level renderer state and backend binding.
- `vg_path`: retained path command buffer (`move`, `line`, `cubic`, `close`).
- Frame lifecycle: all draw calls must happen between `vg_begin_frame` and `vg_end_frame`.
- Backends: currently `VG_BACKEND_VULKAN`.

## Result Codes

- `VG_OK`: success.
- `VG_ERROR_INVALID_ARGUMENT`: invalid pointer/state/value.
- `VG_ERROR_OUT_OF_MEMORY`: allocation failure.
- `VG_ERROR_BACKEND`: backend/Vulkan-specific failure.
- `VG_ERROR_UNSUPPORTED`: feature not available in current backend/config.

Use `vg_result_string(vg_result)` for diagnostics.

## Data Types

### `vg_vec2`

2D point in pixel space.

### `vg_color`

RGBA color in normalized floats `[0, 1]`.

### `vg_rect`

Axis-aligned rectangle in pixel space.

- `x`, `y`: top-left corner.
- `w`, `h`: width/height in pixels.

### `vg_stroke_style`

- `width_px`: stroke width in pixels; must be `> 0`.
- `intensity`: emission multiplier; must be `>= 0`.
- `color`: base stroke color.
- `cap`: `VG_LINE_CAP_BUTT | VG_LINE_CAP_ROUND | VG_LINE_CAP_SQUARE`.
- `join`: `VG_LINE_JOIN_MITER | VG_LINE_JOIN_ROUND | VG_LINE_JOIN_BEVEL`.
- `miter_limit`: must be `> 0`.
- `blend`: `VG_BLEND_ALPHA | VG_BLEND_ADDITIVE`.

### `vg_frame_desc`

- `width`, `height`: render target size in pixels.
- `delta_time_s`: frame delta, optional for effects/time-based systems.
- `command_buffer`: backend command stream hook.

For Vulkan backend:
- Pass a valid recording `VkCommandBuffer` cast to `void*` when using GPU submission path.
- Leave `NULL` to skip Vulkan draw recording and use CPU debug rasterization APIs only.

### `vg_retro_params`

- `bloom_strength`
- `bloom_radius_px`
- `persistence_decay`
- `jitter_amount`
- `flicker_amount`

These currently drive preview-path retro effects and are also stored in context/backend state.

### `vg_backend_vulkan_desc`

All Vulkan handles are passed as opaque `void*` in the public header and interpreted internally as Vulkan handles.

- `instance`: `VkInstance`
- `physical_device`: `VkPhysicalDevice`
- `device`: `VkDevice`
- `graphics_queue`: `VkQueue`
- `graphics_queue_family`: queue family index used for command pool/binding assumptions.
- `render_pass`: `VkRenderPass` used by internal pipeline path.
- `vertex_binding`: vertex binding index for internally bound vertex buffer.
- `max_frames_in_flight`: currently stored and defaulted; future frame resource sizing hook.

## Context API

### `vg_context_create(const vg_context_desc* desc, vg_context** out_ctx)`

Creates a context.

Requirements:
- `desc != NULL`
- `out_ctx != NULL`
- `desc->backend` supported (`VG_BACKEND_VULKAN`)

Returns:
- `VG_OK` on success.
- `VG_ERROR_BACKEND` if backend init fails.
- `VG_ERROR_UNSUPPORTED` for unsupported backend.

### `vg_context_destroy(vg_context* ctx)`

Destroys context and backend resources.

Behavior:
- Accepts `NULL`.
- For Vulkan backend, tears down internal GPU resources/pipelines if initialized.

## Frame API

### `vg_begin_frame(vg_context* ctx, const vg_frame_desc* frame)`

Begins a frame.

Requirements:
- `ctx != NULL`, `frame != NULL`
- `frame->width > 0`, `frame->height > 0`
- no active frame already

### `vg_end_frame(vg_context* ctx)`

Ends a frame and submits backend work.

For Vulkan backend:
- validates recorded draw ranges
- if Vulkan path is configured and `frame.command_buffer` is non-null:
  - uploads staged vertex data to a host-visible `VkBuffer`
  - binds vertex buffer
  - sets viewport/scissor
  - records `vkCmdDraw` for each recorded draw
  - uses internal pipeline path when available

## Retro Parameters API

### `vg_set_retro_params(vg_context* ctx, const vg_retro_params* params)`

Stores and forwards retro params to backend state.

### `vg_get_retro_params(vg_context* ctx, vg_retro_params* out_params)`

Reads current retro params.

## Path API

### `vg_path_create(vg_context* ctx, vg_path** out_path)`

Creates a path object owned by `ctx`.

### `vg_path_destroy(vg_path* path)`

Destroys a path and its internal command buffer.

### `vg_path_clear(vg_path* path)`

Clears path commands.

### `vg_path_move_to(vg_path* path, vg_vec2 p)`
### `vg_path_line_to(vg_path* path, vg_vec2 p)`
### `vg_path_cubic_to(vg_path* path, vg_vec2 c0, vg_vec2 c1, vg_vec2 p1)`
### `vg_path_close(vg_path* path)`

Appends commands.

Current backend behavior notes:
- `move_to` starts a new subpath.
- `close` flushes a closed subpath for stroke generation.
- cubic curves are flattened with fixed subdivision.

## Draw API

### `vg_draw_polyline(vg_context* ctx, const vg_vec2* points, size_t count, const vg_stroke_style* style, int closed)`

Records a stroked polyline into backend draw buffers.

Requirements:
- active frame
- `points != NULL`, `count >= 2`
- valid `style`

### `vg_draw_path_stroke(vg_context* ctx, const vg_path* path, const vg_stroke_style* style)`

Strokes a `vg_path` by flattening commands into one or more polylines.

Requirements:
- active frame
- `path` belongs to `ctx`
- valid `style`

### `vg_measure_text(const char* text, float size_px, float letter_spacing_px)`

Returns text width in pixels for the built-in stroke font.

Notes:
- Supports ASCII-ish UI text (uppercase letters, digits, punctuation used by demo UI).
- Lowercase input is normalized to uppercase.
- Supports `\n` for multi-line width calculation (returns max line width).

### `vg_draw_text(vg_context* ctx, const char* text, vg_vec2 origin, float size_px, float letter_spacing_px, const vg_stroke_style* style, float* out_width_px)`

Renders stroke text using the built-in line font.

Requirements:
- active frame
- valid `style`
- `text != NULL`
- `size_px > 0`

Notes:
- `origin` is text top-left anchor.
- `out_width_px` is optional and reports rendered width.

### `vg_draw_rect(vg_context* ctx, vg_rect rect, const vg_stroke_style* style)`

Draws a stroked rectangle primitive.

Requirements:
- active frame
- `rect.w > 0`, `rect.h > 0`
- valid `style`

### `vg_draw_button(vg_context* ctx, vg_rect rect, const char* label, float label_size_px, const vg_stroke_style* border_style, const vg_stroke_style* text_style, int pressed)`

Draws a stroked vector button (rectangle + centered stroke text).

Requirements:
- active frame
- valid rectangle and styles
- `label != NULL`
- `label_size_px > 0`

### `vg_draw_slider(vg_context* ctx, vg_rect rect, float value_01, const vg_stroke_style* border_style, const vg_stroke_style* track_style, const vg_stroke_style* knob_style)`

Draws a horizontal slider primitive:
- outer rectangle
- horizontal track line
- rectangle knob at normalized `value_01`

Requirements:
- active frame
- valid rectangle and styles
- `value_01` is clamped to `[0, 1]`

## Debug Raster API

### `vg_debug_rasterize_rgba8(vg_context* ctx, uint8_t* pixels, uint32_t width, uint32_t height, uint32_t stride_bytes)`

CPU raster fallback for preview/debug.

Requirements:
- active frame
- `pixels != NULL`
- `width > 0`, `height > 0`
- `stride_bytes >= width * 4`

Behavior:
- rasterizes staged triangles into RGBA8
- applies blend modes
- applies retro effects (bloom/flicker/jitter) based on current params

## Backend Integration: Vulkan

### Minimum setup for GPU draw recording

1. Create Vulkan instance/device/swapchain/render pass in app.
2. Create `vg_context` with `VG_BACKEND_VULKAN` and populate Vulkan handles in `vg_backend_vulkan_desc`.
3. Per frame, pass recording `VkCommandBuffer` through `vg_frame_desc.command_buffer`.
4. Call `vg_begin_frame` -> draw calls -> `vg_end_frame` while render pass is active.

### Internal pipeline path

Internal pipeline is available when all are true:
- Vulkan is found at build time.
- shader tools are available (`glslangValidator`, `xxd`) at build time.
- `render_pass` is provided in backend desc.

If this path is unavailable, app can still bind its own graphics pipeline before `vg_end_frame` draw recording.

### Vertex input contract (internal pipeline)

- binding: `vg_backend_vulkan_desc.vertex_binding`
- location `0`: `vec2` (`VK_FORMAT_R32G32_SFLOAT`)
- primitive topology: triangle list

## Known Limitations

- API/ABI not stabilized yet.
- No fills yet; stroke-focused path.
- Curve flattening is fixed-step, not adaptive.
- GPU upload currently uses host-visible memory; no staging+device-local path yet.
- No internal swapchain or render pass management; app owns frame orchestration.
- Persistence in Vulkan output is app-driven (see `examples/demo_vk_sdl.c` for a render-pass `LOAD` + fullscreen fade pattern).
- Demo bloom in Vulkan example uses a post-process composite path (offscreen scene target + bloom target + fullscreen composite).
- Built-in text uses an embedded stroke font table; loading `.ttf` line fonts is not implemented yet.

## Quick Usage Skeleton

```c
vg_context* ctx = NULL;
vg_context_desc desc = {0};
desc.backend = VG_BACKEND_VULKAN;
desc.api.vulkan.instance = (void*)instance;
desc.api.vulkan.physical_device = (void*)physical_device;
desc.api.vulkan.device = (void*)device;
desc.api.vulkan.graphics_queue = (void*)graphics_queue;
desc.api.vulkan.graphics_queue_family = graphics_qf;
desc.api.vulkan.render_pass = (void*)render_pass;
desc.api.vulkan.vertex_binding = 0;
vg_context_create(&desc, &ctx);

vg_frame_desc frame = {
    .width = width,
    .height = height,
    .delta_time_s = dt,
    .command_buffer = (void*)cmd
};
vg_begin_frame(ctx, &frame);
vg_draw_polyline(ctx, pts, count, &style, 0);
vg_end_frame(ctx);
```
