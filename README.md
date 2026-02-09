# vectorgfx

A C vector graphics library scaffold aimed at Linux + Vulkan with a retro vector-display look.

## Current status

This repository currently provides:
- a public C API in `include/vg.h`
- detailed API docs in `docs/api.md`
- lifecycle/path containers plus input validation in `src/vg.c`
- internal backend dispatch (`src/vg_internal.h`) with Vulkan hook wiring
- backend/effects module boundaries (`src/backends/vulkan`, `src/fx`)
- a minimal API demo (`examples/demo.c`)
- an SDL real-time preview (`examples/demo_sdl.c`)
- a Vulkan + SDL end-to-end example (`examples/demo_vk_sdl.c`)
- basic UI/text primitives: `vg_draw_text`, `vg_draw_rect`, `vg_draw_button`, `vg_draw_slider`
- fill primitives: `vg_fill_convex`, `vg_fill_rect`, `vg_fill_circle`
- CRT profile API with presets: `vg_make_crt_profile`, `vg_set_crt_profile`, `vg_get_crt_profile`

`vg_draw_polyline`/`vg_draw_path_stroke` now generate CPU-side stroke triangle meshes in the Vulkan backend.
A frame-local draw-command buffer is recorded.
When Vulkan handles and `vg_frame_desc.command_buffer` are provided, vertices are uploaded to a real `VkBuffer` and `vkCmdDraw` calls are emitted.
If `vg_backend_vulkan_desc.render_pass` is supplied, the backend creates/binds an internal minimal graphics pipeline (alpha + additive variants).
If internal pipeline creation is unavailable, caller-managed pipeline binding is still supported.
`vg_debug_rasterize_rgba8` provides a CPU fallback preview path for immediate visual iteration.
The SDL preview currently applies bloom, flicker, jitter, and persistence using `vg_retro_params`.
The Vulkan + SDL example now includes persistence by retaining swapchain contents and applying a fullscreen fade stroke each frame.
The Vulkan + SDL example also applies flicker/jitter and a real bloom composite pass (scene RT -> bloom RT -> swapchain composite).
The Vulkan + SDL example now includes an on-screen debug panel (FPS + live retro parameter tuning) driven by vector button/slider primitives.
The Vulkan backend submission path batches recorded draws by blend mode to reduce pipeline switches.
The Vulkan + SDL example includes multiple visual test scenes (keys `1-5`) and a teletype overlay with per-character beeps (`R` to replay).

Text rendering currently uses an embedded line-stroke font table in the library.
`fonts/Vectorb.ttf` is available in-tree as a source asset for future font-import work.

## Build

```sh
cmake -S . -B build
cmake --build build
./build/vg_demo
./build/vg_demo_sdl
./build/vg_demo_vk_sdl
```

## Suggested implementation order

1. Vulkan backend init (device resources, frame contexts, descriptor pools)
2. Polyline stroke renderer (distance field / analytic AA in fragment shader)
3. Path flattening for cubic curves on CPU or compute
4. Glow/bloom pass in HDR
5. Persistence buffer with configurable decay
