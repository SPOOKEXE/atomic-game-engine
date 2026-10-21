# TornadoSim demo and GPU particle field

## Scope

The original port plan targeted a close native port of a C++ tornado simulation: reusable analytical wind, fixed-tick body and joint damage, cloud and audio layers, and reference captures. The current implementation is a scripted `.aworld` demonstration with a generic GPU particle field. The old plan's proposed engine-wide storm model and native physics components are not the interface the demo uses.

## Current demo

[`TornadoSim.aworld`](../mono.engine/examples/assets/worlds/TornadoSim.aworld) carries its server, client, shared Luau modules, authored scene, and controls. `StormControls` defines presets and parameter bounds. `StormField` owns the script-side storm state and sampling functions. The server validates control requests, advances the field, applies body forces and scenery damage through script component schemas, and publishes `Tornado.State` on the authored storm anchor. The world marks that schema as replicated. The client uses the replicated state for presentation, field visualization, audio, camera response, and controls.

The demo's force, joint, and vegetation records are ordinary script-defined components named `Tornado.Response`, `Tornado.Link`, and `Tornado.Vegetation`. Luau reads and writes those records directly; the world does not depend on native storm component registration or a native storm force pass.

## Generic GPU particles

The storm visuals use the general `scene::GpuParticleField` component and its `GpuParticleField` instance class. The API accepts a layer mask, requested population, local bounds, velocity response, and three generic visual styles. Each style has a colour, opacity, size, and constant acceleration.

`SetSpawnSamples` accepts a packed buffer of bounded initial conditions: local-space position, lifetime, velocity, and layer index. These samples seed and recycle the device-local population. The authored samples and style settings are saved; live particle positions stay on the GPU and are not written into world snapshots. The same API can drive effects unrelated to tornadoes.

## Validation

The world-level suite `engine.examples.tornado-sim` checks that the authored asset carries its script sources, controls, scenery, audio, and GPU field setup. `engine.scene.gpuparticlefield` covers authored field validation, spawn-sample encoding, and supported population presets. `engine.render.gpuparticlefieldgpu` covers device execution and rendering behavior.

GPU population size depends on device memory. The demo exposes fixed presets and the field retains its previous working population if a requested allocation cannot be made. Visual comparison with the original standalone Vulkan program remains renderer- and hardware-dependent.

## Historical comparison note

An earlier comparison recorded a 1200-frame mature-funnel capture at 1440 by 900 pixels. It found differences in silhouette texture and scenery between the standalone Vulkan reference and the engine renderer. That note records the earlier comparison target; it does not claim pixel parity for the current scripted world.
