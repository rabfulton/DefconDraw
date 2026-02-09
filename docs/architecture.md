# Architecture Notes

## Layers

- `vg_core` (`src/vg.c`): public API, handles, command validation, frame sequencing.
- `vg_vk` (`src/backends/vulkan`): GPU resource management and drawing pipelines.
- `vg_fx` (`src/fx`): retro display model passes (bloom, persistence, flicker/jitter).
- CPU debug preview: backend can rasterize staged triangles to RGBA8 for fast iteration.

## API design goals

- Opaque handles in public headers for ABI stability.
- Explicit frame boundaries (`vg_begin_frame` / `vg_end_frame`).
- Style-driven strokes with width/intensity for retro display tuning.
- Backend abstraction in `vg_context_desc` for future non-Vulkan targets.

## Next technical steps

- Improve curve flattening with adaptive subdivision (currently fixed-step CPU flattening).
- Implement `vg_fx` passes (HDR bloom + persistence history texture).
- Move Vulkan uploads from host-visible memory to staging + device-local buffers.
