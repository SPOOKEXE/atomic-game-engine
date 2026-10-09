# demo projects

The staged sample set currently has 72 Luau scripts, 3 JavaScript scripts and
2 `.aworld` files. Sources live under
[`mono.engine/examples/assets`](../mono.engine/examples/assets). Use the
examples to inspect behavior, then run them through the same headless client
that ships.

```console
just preset=dev demo-check
just preset=dev demo-check 240
```

`demo-check` builds the client, runs every staged top-level Luau and JavaScript
script plus every staged `.aworld` file, and writes a BMP, full client log and
`results.json` under a unique `.cache/build/<preset>/demo-check/` directory.
It uses Vulkan with dummy audio, captures at 640 by 360, and checks the exit
status, error/critical/heartbeat log markers and BMP header, pixel format,
payload and dimensions. Ordinary runs have a 45-second limit. `StressPhysics`
and `PbrTextureUniqueStress` have a 600-second limit. The 4K texture stress
also requests its documented 9,216 MiB texture budget.

The bench-preset Vulkan capture sweep passed all 77 staged rows at 120 frames
each with dummy audio and a 60 FPS cap. All 77 captures received visual review.
The captures and `results.json` are in
`.cache/build/bench/demo-check/20261009T072153Z-1658443/`.

A focused rerun with the current checker passed all 7 DataFactory, image-buffer
and particle rows. This includes explicit `--data-factory` handling. Its
results are in
`.cache/build/bench/demo-check/20261009T073439Z-1687610/results.json`.

For focused reruns, call the script directly:

```console
python3 scripts/demos/check-demos.py --build .cache/build/dev \
  --only '^(ImageBuffers|Particles)\.luau$' --frames 240 --timeout 60
```

The headless sweep checks that each demo starts, runs and produces a valid
capture. It does not compare rendered pixels or prove interaction behavior.
Open captures for visual review when the appearance matters. Use the focused
tests and real client checks for behavior that a startup capture cannot prove.

## image and particle examples

- [`ImageBuffers.luau`](../mono.engine/examples/assets/scripts/ImageBuffers.luau)
  shows raw RGBA8, PNG and JPEG import paths, then assigns images to labels,
  mesh texture slots, a `Texture` and a particle emitter. See
  [IMAGE_BUFFERS.md](IMAGE_BUFFERS.md) for the format and size contract.
- [`EditableImage.luau`](../mono.engine/examples/assets/scripts/EditableImage.luau)
  draws into a live image and applies its `ContentId` to a mesh part.
- [`Meshes.luau`](../mono.engine/examples/assets/scripts/Meshes.luau) and
  [`MeshGrid.luau`](../mono.engine/examples/assets/scripts/MeshGrid.luau)
  include a deliberately missing texture. Its purple checkerboard marks a
  known missing asset; it is separate from imported meshes whose textures are
  still loading.
- [`Assets.luau`](../mono.engine/examples/assets/scripts/Assets.luau) uses the
  built-in `engine.Checker` as an intentional placeholder while content loads.
- [`ParticleFlipbooks.luau`](../mono.engine/examples/assets/scripts/ParticleFlipbooks.luau)
  compares playback modes using the packaged `effects/fox_dance.atex` atlas.
  [`Particles.luau`](../mono.engine/examples/assets/scripts/Particles.luau)
  includes both a feature row and a large emitter grid.
- [IMAGEGRAPH_LIVE.md](IMAGEGRAPH_LIVE.md) documents named source-path inputs
  and ordinary image outputs. A string parameter bound to `image.source.path`
  can receive an `EditableImage.ContentId`; without an instance override, the
  source path uses the signed graph's authored parameter default. The focused
  signed integration check exercises graph output in particles and other image
  consumers.

`PbrTextureUniqueStress.luau` is a memory stress scene, not a normal sample.
Its 100 unique 4096-square RGBA8 images account for about 6.25 GiB before
renderer allocations. The checker sets the larger texture budget and gives it
a longer run limit when the full staged set runs.
