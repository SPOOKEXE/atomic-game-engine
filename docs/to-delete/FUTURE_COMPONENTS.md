# Render work awaiting a dedicated plan

The detailed plans for `scene::Skeleton`, `scene::Bone`,
`scene::AnimationClip`, `scene::Animator`, and `scene::AnimationTrack` live in
[the character plan](character-system.md). `scene::Constraint` lives in [the
physics plan](physics-expansion.md), and `scene::Terrain` lives in [the terrain
plan](terrain-system.md). Each destination records why the type registers before
`ecs::Components::Seal()` even while later runtime work remains unwired.

The render work below remains here because it has no dedicated lower-case
future-work plan.

## Mesh LOD

LOD selection targets quad utilization. `scene::SelectLevel` compares pixels of
projected area per triangle and never a distance. A tower and a coffee cup at
the same distance cover different areas, so a distance ladder either overdraws
the cup or underserves the tower. The target remains per view, field of view,
and resolution.

`render::AutomaticMeshLodUploader` builds and publishes `Decimated` and
`Reduced` ladders for `scene::LODAuto` rows. `scene::ResolveMeshLOD` combines
those artifacts with `scene::LODCustom` overrides into the derived
`LevelOfDetail` draw snapshot, and `SelectLevel` chooses its `LevelMesh` for a
view.

The selected level is never stored. It varies between views, including mirrors
in the same frame. `LODAuto` and `LODCustom` own the authored ladder; the
resolved `LevelOfDetail` value exists only for draw selection.

## `scene::Atmosphere` and `scene::Clouds`

There is no `Fog` type. The Lighting service authors `FogColor`, `FogStart`, and
`FogEnd`; `LightingOf` resolves them into `WorldLighting`, and the view
recording sends them to the renderer. A separate fog component would create a
second answer to a world's distance fade.

Atmosphere supplies scattering that linear fog cannot express. `Atmosphere` and
`Clouds` are per-world presentation state. They never reach physics, character
control, or queries, so render graphs may select their implementation per
platform without changing simulation.

The render graph reads the `Air` and `Sky` members of `scene::WorldLighting`.
Dynamic ambient occlusion remains a render-pass parameter: strength, radius,
and sample count are graph settings, not world-authored component data.
