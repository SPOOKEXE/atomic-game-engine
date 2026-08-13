# mono.engine/resources — module invariants

L11 · `client`. The engine's built-in GLSL, and the one name that says where the
build stages it.

## This module is its `shaders/` directory

The C++ here is a single path join. That is not an oversight to be filled in:
the module exists so the engine's default shaders have a home that is not inside
one of their consumers, and everything else it could grow would be something a
consumer already owns better.

What that buys is a rule the build enforces rather than a convention. A program
stages the shaders of every module it links, so `<program>/shaders/resources/`
is present exactly when something linked `Engine::resources` — and a second
module wanting `opaque.frag` links it too instead of reaching sideways into
another module's source directory.

## `client`, because GLSL is a client-tier fact

`_mono_add_shaders` fails the configure when a module owns shaders and no
`glslc` was resolved, and `glslc` is only resolved when `MONO_BUILD_CLIENT` is
on. A `shared` module owning the same files would break the `server` preset on a
machine with no shader compiler, and would stage a `shaders/` directory into
`server/` — which `mono.server/AGENTS.md` names as the visible symptom of a
link-line mistake, and the Justfile checks for.

## The light cap is read from `render`, at configure time

`MAX_SCENE_LIGHTS` sizes `LightUniforms` in `render/Renderer.hpp` and
`MAX_LIGHTS` bounds the loop in `opaque.frag`. The build reads the header and
passes the value as a `-D`, so the shader carries no literal of its own to
drift.

That reach is upward and stays a **file read**, never a link edge: the value
belongs beside the struct it sizes, and moving the declaration down here would
put it away from the code that has to agree with it. If that declaration moves
or changes shape, the regex in `CMakeLists.txt` moves in the same commit — it is
a `FATAL_ERROR`, so the build says so.

## A shader a game author writes is a different thing

None of those are in this repository. `render::ShaderCompiler` compiles them at
runtime and a failure there is a diagnostic string, not a build failure —
`mono.engine/render/AGENTS.md` carries the split. Nothing authored outside the
engine belongs in this directory.

## What is deliberately not here

- **No device, no SDL, no pipeline.** This module never opens the files it
  names. `render` decides what a shader is bound to, and a consumer that wanted
  this module to load one for it would be asking for a renderer.
- **No baked meshes or textures.** The engine's default shapes and its missing
  texture are *generated* — `assets::MakeBuiltin` and `render::MissingTexture` —
  and a checked-in file of the same geometry would be a second copy of a fact
  the code already owns, free to drift from it. A default that has to be a file
  because nothing can generate it may live here; one that can be generated
  should not be.
