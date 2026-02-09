# Defcon Draw

Defcon Draw is a C vector graphics library for Linux with a Vulkan backend and a CRT-inspired rendering style for retro games, tools, and UI.

## Screenshot Gallery

### Meters / Dashboard
![Meters dashboard](screenshots/meters.png)

### Debug UI
![Debug UI](screenshots/debug_ui.png)

### Wireframe Cube
![Wireframe cube](screenshots/cube.png)

### Surface Plot
![Surface plot](screenshots/surface.png)

## Features

- Public C API in `include/vg.h`
- Vulkan backend with alpha/additive batching
- CPU fallback raster path (`vg_debug_rasterize_rgba8`)
- Retro CRT profile system with tunable bloom, persistence, jitter, flicker, scanlines, vignette, and barrel distortion
- Stroke text + boxed text + wrapped text helpers
- Stroke and fill primitives (`polyline`, `path`, `rect`, `convex fill`, `circle fill`)
- Reusable immediate-style debug UI (`vg_ui`)
- Extended widgets (`vg_ui_ext`): meters, line graphs, bar graphs, histogram, pie/donut charts, history buffers
- Multi-scene Vulkan demo with teletype effect, profile save/load, and live parameter tuning

## Documents

- API reference: `docs/api.md`
- Architecture notes: `docs/architecture.md`
- Brainstorm / roadmap notes: `brainstorm.md`

## Build Instructions

```sh
cmake -S . -B build
cmake --build build -j
```

Run demos:

```sh
./build/vg_demo
./build/vg_demo_sdl
./build/vg_demo_vk_sdl
```

Vulkan demo controls:

- `TAB`: show/hide debug UI
- `1..7`: switch scene
- `UP/DOWN`: select UI row
- `LEFT/RIGHT`: adjust selected value (key-hold repeat supported)
- `R`: replay teletype message
- `F5`: save profile
- `F9`: load profile
