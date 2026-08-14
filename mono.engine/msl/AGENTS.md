# mono.engine/msl — module invariants

L11 · `client`. SPIR-V in, Metal Shading Language out, and the descriptor
indices `SDL_CreateGPUShader` documents.

## It exists because there are two callers, not because `render` was crowded

`mono.tools/shadercross` translates the built-in shaders during the build.
`render::ShaderCompiler` translates a `ShaderScript` while the engine is
running. Those are two programs at two times, and a translation that lived
inside `render` would either make a build tool link the renderer or make the
build-time and runtime paths two different pieces of code.

Two pieces of code is the failure that matters. A `ShaderScript` and
`unlit.frag` both end up in the same pipeline slots, so a disagreement about
which texture is `[[texture(0)]]` shows up on macOS as one of them sampling the
other's map — and on Linux as nothing at all.

## The index assignment is the module

SPIRV-Cross numbers resources in the order it walks the module's ids.
`SDL_CreateGPUShader` numbers them by kind and then by descriptor set and
binding: sampled textures then storage textures, uniform buffers then storage
buffers, a sampler taking the index of the texture it belongs to. Those agree
for a shader with one of everything and disagree for every shader with two —
measured on `opaque.frag`, where the automatic assignment put `beamMap`, the
last texture in set 2, at `[[texture(0)]]`.

So `Translate` tells SPIRV-Cross every index rather than reading back what it
chose. **If that rule is edited here it is edited in `SDL_gpu.h` too, or it is
wrong** — the documentation of `SDL_CreateGPUShader` and
`SDL_CreateGPUComputePipeline` is the source, and both are quoted in
`Translate.cpp`.

`mono.tools/shadercheck` states the same rule a second time, on purpose. It
derives what each resource *should* land on and reads the emitted `.msl` back to
see what it did, so the two ends disagreeing is a failed `just check` rather
than a shared mistake. Do not make one call the other: a checker that links the
thing it checks is a checker that agrees with it by construction.

## No SPIRV-Cross type in a public header

`Translation` is a string, a bool and a diagnostic. `render` and a build tool
both call this without acquiring a translator's API, which is the same rule
`render::ShaderCompiler` follows for shaderc and for the same reason.

## Nothing here has ever run on a Mac

The translation is verifiable from Linux and the *execution* of what it produces
is not. `just shader-check` checks structure, entry point and binding indices;
there is no Metal compiler on this platform and no Metal device, so nothing in
this module or its suite claims a shader runs. `docs/DEFERRED.md` D00001 holds
what is left and what would answer it.

## The suite carries a compiled fixture, and that is deliberate

SPIRV-Cross needs a complete, valid module — types, a function body, a return —
so the hand-assembled instruction streams `mono.tools/shadercheck` builds are no
use here. `tests/Translate.cpp` embeds one shader compiled by `glslc`, with the
GLSL it came from and the command that regenerates it in the same file. A
fixture whose provenance is in the file beside it is not a magic blob.
