# TornadoSim engine port plan

## Target

Port the C++ program at `/home/declan/Documents/GitHub/TornadoSim` as closely as the engine renderer and hardware allow. Keep its analytical wind field, simulation behavior, storm controls, audio, and visual layers. The wind and damage systems must be reusable by ordinary engine scenes. The reference's buildings, vegetation, vehicles, and terrain belong to a built-in Luau scripted `.aworld` demo, loaded like `BladeborneDemo.aworld`. Its controls appear in an in-game panel.

The existing `TornadoSim.luau` is a smaller demonstration. It has three EF presets, a simplified field, particle emitters, placed volumes, clouds, rain, lightning, and a short status panel. It does not implement the reference's full EF0 to EF5 and Q0 to Q5 controls, two-cell field, structure connectivity, material damage, spatial collision, cloud density octree, spatial audio, environment streaming, or million-scale GPU particle simulation.

## Implementation

1. Record the reference baseline. Build the C++ repository, run its CTest and smoke tests, capture its default and mature-funnel views, and record the 1M and available larger-preset profiles on the current GPU. Keep generated profiles under build directories.
2. Add the reusable storm model under `mono.engine`: sanitized parameters, EF and Q presets, a prepared two-cell analytical field, lifecycle, visibility and damage queries, and deterministic turbulence. Match the reference equations and tests. Expose authored storm parameters and read-only field queries to Luau so the scene script can configure and query them while the engine runs fixed-tick physics.
3. Connect the field to engine physics and effects. Apply mass, area, and drag-based forces to affected bodies at a fixed tick; implement attachment failure, material joints, structure connectivity, broadphase contacts, bounces, and vegetation bend. Keep the simulation deterministic and avoid world-crossing pointers.
4. Port the visible storm layers. Use engine particle emitters for rain, dirt, debris, and mist; use placed fog volumes and cloud/atmosphere lighting for condensation and the anvil. Add a reusable GPU compute particle field for the reference's optional 262k through 50M cosmetic presets, 12 depth slices, and device-local simulation without CPU readback. The default remains 1M. Failed allocation retains the previous working preset; integrated GPUs may use the reference's phased background update. Keep all backend work in the engine's SDL GPU path.
5. Port cloud density generation as sparse, adaptive data that can feed the renderer and gameplay visibility queries. Reuse the same analytical wind sample for clouds, rain, debris, damage, the vector inset, and audio. Add procedural wind, rain, debris, circulation, and delayed thunder audio through the engine audio graph.
6. Add `TornadoSim.aworld` with embedded server, client, and shared Luau sources in the same format as `BladeborneDemo.aworld`. The scripts author the reference scene and its in-game controls: EF0 to EF5, Q0 to Q5, Custom, parameter sliders, layer switches, pause/reset/lifecycle, camera movement and shake, streamed scenery, three material-aware buildings, trees, signs, roof panels, vehicles, and a field/vector inset. Preserve the original's default camera and storm composition for visual comparison. Retire the smaller standalone `TornadoSim.luau` after the world replaces it.

## Validation

- Compare field samples, presets, lifecycle, damage, cloud density, and fixed-step outcomes against the C++ reference with deterministic fixtures.
- Test overlapping local volumes, clouds, sun rays, lightning, rain, and debris in scene captures. Compile and execute the compute shader path, including preset resizing, failure fallback, and zero readback.
- Run headless smoke and GPU tests at supported presets, plus the existing reference visual capture and matched engine camera captures. Compare the funnel shape, motion, cloud deck, rain veil, ground mist, lightning, and scenery in default and mature views.
- Add a `just` job for every new benchmark, print benchmark results to the terminal, and keep build artifacts under the build directory. Measure CPU fixed-step, collision scaling, GPU compute, frame time, and memory in `release` with the device and preset named.
- Use Studio to open, edit, run, and play the completed demo. Exercise storm controls and object classes, then fix issues the session reproduces.

## Review gates

The original uses direct Vulkan while the engine uses SDL GPU. The same appearance and behavior are the target, but exact pixels and identical GPU timings are not portable across these renderers. The 50M preset requires about 1.6 GiB in the reference and must stay optional with a clear allocation failure path.

The user approved this plan. Lighting capabilities and their stress tests are tracked separately.

## Verification record

The C++ reference passes all three CTest suites. Engine fixtures cover field samples, EF and Q presets, lifecycle, damage, and the 1M default. The Vulkan particle test covers depth-sliced drawing, preset resizing, and a forced 50M allocation failure that retains the previous working field without a CPU particle readback.

Default and mature funnel captures were compared; the matched mature capture used 1200 frames at 1440 by 900 pixels. The mature engine camera was aligned to the reference horizon and funnel framing. Bounded, depth-ordered condensation drawing and broader billow blending removed the detached cap and made the middle of the funnel more continuous. The remaining silhouette texture and scenery differ between the SDL GPU engine scene and the direct Vulkan reference.
