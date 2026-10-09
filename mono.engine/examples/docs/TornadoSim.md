# TornadoSim

`TornadoSim.aworld` keeps the tornado model in its shared Luau module. The server owns its position, EF and Q presets, lifecycle, translation, body drag, joint damage and vegetation bend. It writes the bounded state to the replicated `Tornado.State` component on `StormAnchor`. The client samples that same row for its panel, audio, lightning and camera.

The authored `Tornado.Response`, `Tornado.Link` and `Tornado.Vegetation` components describe world-specific gameplay values. They are declared with `World:DefineComponent`; no engine physics class is required. Damageable bodies use generic applied force, joints lose integrity from their connected bodies' calculated drag, and anchored vegetation bends toward the sampled horizontal wind.

Cloud, rain and debris presentation uses the existing volume, emitter and sound paths. The server feeds deterministic, field-derived spawn samples and three styles into `GpuParticleField`, then sets its live vector field and requested count. The supported controls retain the 262K through 50M presets. The visual particle path is bounded to 8192 authored spawn samples; gameplay calculations remain in Luau and do not depend on the GPU field.

The Luau fixture test loads the module embedded in the world and checks deterministic velocity and damage samples, lifecycle values, translation and visibility behavior. Use the normal example-world runner to play the client and server together.
