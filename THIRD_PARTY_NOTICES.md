# Third-party notices

This engine is MPL-2.0. It builds against the projects below, each under its own
licence. Every one of them lives in `mono.vendor/` as a git submodule, so the
full licence text ships with the source in `mono.vendor/<name>/`.

All twelve are permissive and compatible with MPL-2.0. **Nothing here is copyleft
beyond MPL-2.0's own file-level scope**, and that is a condition of adding a
dependency rather than a happy accident — see `mono.vendor/AGENTS.md`.

| Library | Licence | Used for | In a shipped game |
|---|---|---|---|
| [SDL3](https://github.com/libsdl-org/SDL) | Zlib | window, input, GPU abstraction | client only |
| [flecs](https://github.com/SanderMertens/flecs) | MIT | ECS storage behind `engine::ecs` | yes |
| [glm](https://github.com/g-truc/glm) | MIT / Happy Bunny | the maths under `core/types` | yes |
| [spdlog](https://github.com/gabime/spdlog) | MIT | logging behind `core::Log` | yes |
| [Tracy](https://github.com/wolfpld/tracy) | 3-clause BSD | the engine profiler | yes, on demand only |
| [Catch2](https://github.com/catchorg/Catch2) | BSL-1.0 | the test framework | no — tests only |
| [shaderc](https://github.com/google/shaderc) | Apache-2.0 | `glslc` at build time, `libshaderc` at runtime | yes, once a caller exists — see below |
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT | editor and tooling UI | studio only |
| [asio](https://github.com/chriskohlhoff/asio) | BSL-1.0 | networking, when `net` exists | yes, once linked |
| [Crypto++](https://github.com/weidai11/cryptopp) | BSL-1.0 (files public domain) | SHA-256 behind `core::Random`, and the test runner's cache | yes |
| [cryptopp-cmake](https://github.com/abdes/cryptopp-cmake) | BSD-3-Clause | the CMake build for Crypto++ | no — build system only |
| [doxygen-awesome-css](https://github.com/jothepro/doxygen-awesome-css) | MIT | the API reference's stylesheet | no — `just docs` only |

shaderc pulls in **glslang** (BSD-3-Clause / Apache-2.0), **SPIRV-Tools**
(Apache-2.0) and **SPIRV-Headers** (MIT-style) through its own `DEPS` file. They
land in `mono.vendor/shaderc/third_party/` and their licences are there.

## What actually ships

The table's last column matters for attribution. A game built with this engine
contains the `yes` rows and not the others:

- **Catch2 is build-time only.** No test framework is in a client or server
  binary.
- **shaderc is both, and only one half ships today.** `glslc` compiles the
  built-in shaders during the build and goes nowhere near a binary.
  `libshaderc` is for the other half: the renderer is a graph, so the shaders
  it runs are not all known ahead of time — a `ShaderScript` whose revision
  changed, a swapped antialias pass, a permutation the demand pass solved for.
  `engine::render::ShaderCompiler` wraps it and `RENDER_PIPELINE.md` §11.9.1 is
  the case for it.

  **Right now the client binary contains none of it.** `render` links shaderc
  privately, but nothing in the client calls the compiler yet, so the linker
  drops every object — measured: 687 shaderc symbols in `test_render`, zero in
  `client`. It arrives in a shipped binary the day the graph renderer calls it,
  which is why the table says "once a caller exists" rather than "no".

  That mechanism, and how to re-check this claim after a version bump, is
  `docs/CPP_LINKER.md`. Two things there bear on this file: dead-stripping
  happens per **object file**, not per function, and a library can have a floor
  it drags in regardless — a program calling only SHA-256 still links 36 of
  Crypto++'s 173 members. So "we only use one function from it" is never on its
  own a reason to leave a notice out. Run the `nm` check.
- **SDL3, shaderc and Dear ImGui are client-side.** A headless server links
  none of them, and the `server` preset does not even configure them — only the
  presentation modules own GLSL, so nothing server-tier needs a compiler for
  it.
- **Tracy is compiled in but on-demand.** It collects nothing until a profiler
  attaches and listens on localhost only.
- **Crypto++ is two rows for one library, and only one of them can ship.**
  `cryptopp` is the library. `cryptopp-cmake` is its build system, vendored
  because upstream ships a GNUmakefile and no CMakeLists — it produces no object
  code, so no binary can contain it and no distribution needs its notice. It is
  listed because the rule here is one entry per submodule, and because a licence
  review that finds an unlisted BSD-3-Clause submodule will stop.

  Crypto++ itself **now ships in both the client and the server.**
  `engine::core::Random` is SHA-256 underneath and `core` is linked by
  everything, so unlike SDL this produces no tier split — there is no build of
  this engine that carries the notice for one and not the other.

  The obligation is still the lightest one in this file. BSL-1.0 exempts
  machine-executable object code from attribution, so a binary-only
  distribution owes nothing; a source distribution owes `License.txt` whole,
  all three licences in it.
- **doxygen-awesome-css is a stylesheet.** It is copied beside the generated
  HTML by `just docs` and is never compiled, linked or staged. It is a submodule
  rather than a copied `.css` for the ordinary reason — a vendored file gets a
  local fix, upstream gets a different one, and nobody finds out.

## If you add one

1. Check the licence is compatible with MPL-2.0 **before** anything else.
   Anything requiring the whole work to be relicensed is not a candidate, and
   finding that out after the code depends on it is expensive.
2. Add it as a submodule under `mono.vendor/`, pinned to a release branch.
3. Add a row here, with what it is used for and whether it reaches a shipped
   binary.
4. Configure it in `mono.build/MonoVendor.cmake`, not in the module that
   consumes it.

`mono.vendor/AGENTS.md` has the rest, including why nothing here is ever copied
into the tree as source.
