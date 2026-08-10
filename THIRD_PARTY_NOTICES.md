# Third-party notices

This engine is MPL-2.0. It builds against the projects below, each under its own
licence. Most live in `mono.vendor/` as git submodules. The standalone fonts and
the compiled-in texture are tracked exceptions, with their licence or
provenance beside the file.

All of them are permissive and compatible with MPL-2.0. **Nothing here is
copyleft beyond MPL-2.0's own file-level scope**, and that is a condition of
adding a dependency rather than a happy accident — see `mono.vendor/AGENTS.md`.

One of them is not a dependency of the build at all. **luau-lsp is a developer
tool**, cloned only when somebody runs `just luau-lsp`; `just setup` walks past
it and no target links it. It brings a second copy of Luau with it, which is why
it is built in a tree of its own — `.gitmodules` carries the reasoning.

| Library | Licence | Used for | In a shipped game |
|---|---|---|---|
| [SDL3](https://github.com/libsdl-org/SDL) | Zlib | window, input, GPU abstraction | client only |
| [glm](https://github.com/g-truc/glm) | MIT / Happy Bunny | the maths under `core/types` | yes |
| [spdlog](https://github.com/gabime/spdlog) | MIT (bundled fmt is MIT) | logging behind `core::Log` | yes |
| [Tracy](https://github.com/wolfpld/tracy) | 3-clause BSD | the engine profiler | yes, on demand only |
| [Catch2](https://github.com/catchorg/Catch2) | BSL-1.0 | the test framework | no — tests only |
| [shaderc](https://github.com/google/shaderc) | Apache-2.0 | `glslc` at build time, `libshaderc` at runtime | yes, once a caller exists — see below |
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT | editor and tooling UI | studio only |
| [asio](https://github.com/chriskohlhoff/asio) | BSL-1.0 | networking, when `net` exists | yes, once linked |
| [Crypto++](https://github.com/weidai11/cryptopp) | BSL-1.0 (files public domain) | X25519, HKDF-SHA256, ChaCha20-Poly1305 and HMAC-SHA256 in `net`; Ed25519 and HMAC-SHA256 in `assets`; PNG CRC/DEFLATE in `bake`; deterministic hashing and the test runner's cache | yes, where linked |
| [cryptopp-cmake](https://github.com/abdes/cryptopp-cmake) | BSD-3-Clause | the CMake build for Crypto++ | no — build system only |
| [BLAKE3](https://github.com/BLAKE3-team/BLAKE3) | CC0-1.0, or Apache-2.0, or Apache-2.0 with LLVM exception | the content hash under `assets` — chunk, asset, bundle and manifest addressing | yes, once linked |
| [Zstandard](https://github.com/facebook/zstd) | **BSD-3-Clause** (dual-licensed; we do not take the GPLv2 option) | compression for content-delivery groups | yes, once linked |
| [doxygen-awesome-css](https://github.com/jothepro/doxygen-awesome-css) | MIT | the API reference's stylesheet | no — `just docs` only |
| [Luau](https://github.com/luau-lang/luau) | MIT (and MIT for the Lua 5.1 it forks) | the Luau script VM and compiler; its analysis library behind `mono.tools/scriptcheck` | yes, where the Luau runtime is linked |
| [luau-lsp](https://github.com/JohnnyMorganz/luau-lsp) | MIT | the editor's Luau language server, from v0.7 | no — never built by this build; `just luau-lsp` builds it separately |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT | Model Context Protocol control messages and glTF import | server/studio control and studio tooling |
| [toml++](https://github.com/marzer/tomlplusplus) | MIT | the Rojo sync's `*.toml` row, from v0.13 | studio only |
| [QuickJS-ng](https://github.com/quickjs-ng/quickjs) | MIT | the JavaScript/TypeScript script VM | yes, where the JavaScript runtime is linked |
| [Inter](https://github.com/rsms/inter) | SIL OFL 1.1 | interface text in the editor and game UI | staged beside programs linking `engine::ui` or `engine::render` |
| [JetBrains Mono](https://github.com/JetBrains/JetBrainsMono) | SIL OFL 1.1 | the monospace typeface the script editor uses, from v0.7 | as above |
| [Roboto](https://github.com/googlefonts/roboto-classic) | SIL OFL 1.1 | the display typeface, from v0.7 | as above |
| [Noto Sans](https://github.com/notofonts/latin-greek-cyrillic) | SIL OFL 1.1 | fallback coverage for editor and game UI | as above; render loads it as a separate face |
| [minimp3](https://github.com/lieff/minimp3) | CC0-1.0 | MP3 decoding behind `engine::audio`, from v0.9 | client only — nothing else links `audio` |

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
  `docs/retired/CPP_LINKER.md`. Two things there bear on this file: dead-stripping
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

  Crypto++ itself **now ships in both the client and the server**, so unlike
  SDL this produces no tier split — there is no build of this engine that
  carries the notice for one and not the other.

  **The reason given here used to be `core::Random`, and that is no longer the
  cause.** The exact archive members are build- and call-graph-dependent. `net`
  pulls the cipher, handshake and admission-cookie code; `assets` is a second
  cause through `Grant`'s HMAC and Ed25519; and `bake` uses the CRC/DEFLATE
  paths. The origin is now wired into `mono.cdn`, so the final membership must
  be rechecked from the current release link rather than copied from an old
  stub-state measurement.

  Worth stating because the wrong cause was load-bearing for a deferred item:
  `D00004` was filed to ask whether `core` should stop linking Crypto++, on the
  strength of this paragraph. The answer is that it would save every shipping
  binary nothing.

  The obligation is still the lightest one in this file. BSL-1.0 exempts
  machine-executable object code from attribution, so a binary-only
  distribution owes nothing; a source distribution owes `License.txt` whole,
  all three licences in it.
- **BLAKE3 is the lightest obligation here, and it is genuinely optional.** It
  is offered under CC0-1.0 *or* Apache-2.0 *or* Apache-2.0 with the LLVM
  exception, and taking the CC0 option means a public-domain dedication with no
  attribution requirement at all. It is listed anyway, because the rule here is
  one entry per submodule and because a licence review that finds an unlisted
  submodule stops regardless of what the licence turns out to say.

  Only `mono.vendor/blake3/c/` is built — the C implementation. The Rust crate
  in the same repository is upstream's reference and is not vendored, compiled
  or shipped.

  It ships wherever `assets` is linked, which by `repo_layout.md` §8 is every
  program: the client, the server, studio, the CLI and the origin. Like
  Crypto++ and unlike SDL, it produces no tier split.
- **Zstandard is dual-licensed, and which half we take matters.** Upstream ships
  two texts: `LICENSE` is BSD-3-Clause and `COPYING` is GPLv2. **We take the
  BSD-3-Clause option**, and that is a decision rather than a formality — the
  GPLv2 option would be incompatible with shipping this inside a game binary
  under MPL-2.0, so a reader who assumed the wrong half would reach the wrong
  conclusion about the whole engine.

  Obligations are the ordinary BSD-3-Clause ones: retain the notice, the
  condition list and the disclaimer, and do not use the copyright holder's name
  to endorse derived products.

  Only the library is built — `ZSTD_BUILD_PROGRAMS` and `ZSTD_BUILD_TESTS` are
  forced off, so the `zstd` command-line tool is neither compiled nor shipped.
  Legacy (0.x) frame support is off too, which is a security decision as much as
  a size one: it is decoder surface parsing origin-supplied bytes that nothing
  here could ever have written.
- **doxygen-awesome-css is a stylesheet.** It is copied beside the generated
  HTML by `just docs` and is never compiled, linked or staged. It is a submodule
  rather than a copied `.css` for the ordinary reason — a vendored file gets a
  local fix, upstream gets a different one, and nobody finds out.

## The typefaces are files, not submodules

Everything else here is a submodule, and `mono.vendor/AGENTS.md` explains why
nothing is copied into the tree as source. The four fonts are the exception, and
it is a deliberate one: each upstream repository is tens of megabytes of sources,
build tooling and every static instance, to obtain one variable `.ttf`. A
submodule of that to take 0.2% of it is a clone everybody pays for.

So `mono.vendor/fonts/` holds the four files and, beside each, the licence text
that upstream ships — which is the whole obligation OFL 1.1 imposes on a binary
distribution. They are **not modified**, and the OFL's reserved-name clause is
therefore not engaged.

They are used at their **default variable instance**, which for all four is the
regular weight. Nothing here drives a weight axis, because stb_truetype — which
is what Dear ImGui rasterises with — does not.

## Art, and the one piece of it that is compiled in

**`mono.engine/render/src/DefaultTexture.inl` is content, not code.** It is one
channel of ambientCG's *Plastic 013 A* colour map, box-filtered from 1024 to 64
and lifted so its brightest texel is white, as a C array. It is what every part
with no material draws as — `render/DefaultTexture.hpp` says why it cannot be
fetched like everything else — so it is in the binary rather than in a store.

**CC0-1.0**, a public-domain dedication: no attribution is required and the row
is here anyway, because "no attribution required" is a licence term and not a
reason to leave the provenance of a shipped asset unrecorded.

`scripts/fetch-materials.py` downloads from the same place and from two others,
all three CC0:

| Source | Licence | What comes from it |
|---|---|---|
| [ambientCG](https://ambientcg.com/) | CC0-1.0 | PBR material sets, and the compiled-in default above |
| [Poly Haven](https://polyhaven.com/) | CC0-1.0 | PBR material sets |
| [cgbookcase](https://www.cgbookcase.com/) | CC0-1.0 | PBR material sets |

**None of that reaches the repository.** The script writes into `.cache/` and
`assetc` bakes into a content store on the machine that ran it, which is where
gigabytes of art belong — `mono.cdn/AGENTS.md` says a monorepo carrying them
makes every clone slow forever. The one exception is the four-kilobyte tile
above, and it is four kilobytes.

## Code that was ported, rather than linked

One entry is neither a submodule nor a file copied in. **The simplex kernels,
the analytical-derivative algebra and the Worley cell search in
`mono.engine/examples/lib/TerrainCore/Noise.luau` were ported from
[Atlas](https://github.com/shmeatley/atlas) (MIT)**, a procedural generation
toolkit for Roblox.

| Source | Licence | What came from it | In a shipped game |
|---|---|---|---|
| [Atlas](https://github.com/shmeatley/atlas) | MIT | simplex, Worley and derivative kernels in the `TerrainCore` example library | only if a game ships that example |

It is a port rather than a dependency because Atlas is a Wally package built
against Roblox: its samplers are objects, its point scattering takes `Vector2`
and its visualiser needs an `EditableImage`. None of that exists here. What was
kept is the maths, and the file names it at the top and says which parts.

**One thing was deliberately not ported: the lattice hashing.** Atlas builds a
shuffled 256-entry permutation table per sampler, which repeats the field every
256 units on every axis; `Noise.luau` predates the port in rejecting exactly
that, so the kernels were rehashed onto its arithmetic hash instead. The
difference is asserted by a test rather than left as a comment.

A port is the awkward case for a notices file, because there is no submodule to
point at and no licence text landing in the tree beside it. The rule this
follows is the one the compiled-in texture above follows: **provenance is
recorded where the work came from, whether or not anything obliges it.**

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
