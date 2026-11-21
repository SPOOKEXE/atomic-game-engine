# Third-party notices

Each third-party dependency, build-time download, shipped asset and ported piece
of code the engine uses, with its licence, pinned version, and whether it
reaches a shipped binary. `scripts/package-release.sh` copies this file into
every release archive.

The engine is MPL-2.0. Every entry below is under its own licence, and all of
them are permissive and compatible with MPL-2.0. **Nothing here is copyleft
beyond MPL-2.0's own file-level scope**, and that is a condition of adding a
dependency (`mono.vendor/AGENTS.md`). The full licence texts ship with each
submodule; `mono.vendor/THIRD_PARTY_NOTICES.md` reproduces the notices.

Versions are the pinned submodule commit as `git describe --tags` reports it,
or the short commit where upstream has no tag at that commit.

---

## Submodules in `mono.vendor/`

| Library | Version | Licence | Used for | In a shipped game |
|---|---|---|---|---|
| [SDL3](https://github.com/libsdl-org/SDL) | `release-3.4.16` | Zlib | window, input, GPU abstraction; built from a patched copy (see below) | client and Studio |
| [MoltenVK](https://github.com/KhronosGroup/MoltenVK) | `v1.4.2` | Apache-2.0 | Vulkan portability over Metal | Apple client and Studio builds |
| [glm](https://github.com/g-truc/glm) | `6f14f47` | MIT or Happy Bunny | the maths under `core/types` | yes |
| [spdlog](https://github.com/gabime/spdlog) | `v1.17.0` | MIT (bundled fmt is MIT) | logging behind `core::Log` | yes |
| [Tracy](https://github.com/wolfpld/tracy) | `v0.14.1` | BSD-3-Clause | the engine profiler; patched copy on Linux | yes, on demand only |
| [Catch2](https://github.com/catchorg/Catch2) | `v3.16.0` | BSL-1.0 | the test framework | no, tests only |
| [Box2D](https://github.com/erincatto/box2d) | `v3.1.0` | MIT | pinned for future 2D physics; not configured or linked | no |
| [shaderc](https://github.com/google/shaderc) | `v2026.4-2-ga8abeb0` | Apache-2.0 | `glslc` at build time, `libshaderc` at runtime | client, once a caller exists (below) |
| [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) | `vulkan-sdk-1.4.357.0` | Apache-2.0 | SPIR-V to MSL: `mono.tools/shadercross` at build time, `Engine::msl` at runtime | client, if `nm` finds it (below) |
| [Dear ImGui](https://github.com/ocornut/imgui) | `v1.92.9b-docking-106-g3bae66c` | MIT | Studio UI, the client debug panel (`Engine::ui`); built from a patched copy | client and Studio |
| [asio](https://github.com/chriskohlhoff/asio) | `asio-1-38-2` | BSL-1.0 | sockets under `net` | yes, where `net` is linked |
| [Crypto++](https://github.com/weidai11/cryptopp) | `7824259` | BSL-1.0 (files public domain) | X25519, HKDF-SHA256, ChaCha20-Poly1305, HMAC-SHA256 in `net`; Ed25519 and HMAC in `assets`; CRC/DEFLATE in `bake`; deterministic hashing | yes |
| [cryptopp-cmake](https://github.com/abdes/cryptopp-cmake) | `bc994d9` | BSD-3-Clause | the CMake build for Crypto++ | no, build system only |
| [BLAKE3](https://github.com/BLAKE3-team/BLAKE3) | `1.8.7-2-g6aab490` | CC0-1.0, or Apache-2.0, or Apache-2.0 with LLVM exception | content hash under `assets` | yes |
| [Zstandard](https://github.com/facebook/zstd) | `v1.5.7` | **BSD-3-Clause** (dual-licensed; we do not take GPLv2) | content-delivery compression | yes |
| [miniz](https://github.com/richgel999/miniz) | `3.1.2` | MIT | ZIP project-package reading and deterministic writing | client, server and Studio through `Engine::game` |
| [doxygen-awesome-css](https://github.com/jothepro/doxygen-awesome-css) | `v2.5.0` | MIT | the API reference stylesheet | no, `just docs` only |
| [Luau](https://github.com/luau-lang/luau) | `0.739` | MIT (and MIT for the Lua 5.1 it forks) | Luau VM and compiler; analysis behind `mono.tools/scriptcheck` | yes, where the Luau runtime is linked |
| [luau-lsp](https://github.com/JohnnyMorganz/luau-lsp) | `1.70.0` | MIT | the editor's Luau language server; built from a patched copy | no, `just luau-lsp` builds it separately |
| [nlohmann/json](https://github.com/nlohmann/json) | `v3.12.0` | MIT | MCP control messages, glTF import, launcher, cdn, discord and tool JSON | client, server, Studio, cdn, launcher |
| [roblox-files](https://github.com/SPOOKEXE/roblox-files) | `v0.1.0-6-g07acfe0` | MIT | Roblox place and model containers for offline import (`Engine::bake`) | no, Studio and content tooling only |
| [toml++](https://github.com/marzer/tomlplusplus) | `1e8829b` | MIT | the Rojo sync's `*.toml` row | Studio only |
| [QuickJS-ng](https://github.com/quickjs-ng/quickjs) | `v0.17.0` | MIT | the JavaScript/TypeScript script VM | yes, where the JavaScript runtime is linked |
| [minimp3](https://github.com/lieff/minimp3) | `ea99364` | CC0-1.0 | MP3 decoding in `Engine::audiocodec` | client and Studio, through `Engine::audio` |
| [ngtcp2](https://github.com/ngtcp2/ngtcp2) | `v1.25.0-102-g4822893` | MIT | QUIC transport under `engine::net::quic` | yes, where `net` is linked |
| [SQLiteCpp](https://github.com/SRombauts/SQLiteCpp) | `3.4.0` | MIT; bundled SQLite is public domain | durable DataStore files; only the bundled SQLite amalgamation is compiled | server and Studio |
| [FreeType](https://github.com/freetype/freetype) | `VER-2-13-3` | FreeType License | TrueType and OpenType table access for GUI shaping | yes, where `Engine::gui` is linked |
| [HarfBuzz](https://github.com/harfbuzz/harfbuzz) | `11.5.1` | MIT ("Old MIT") | OpenType shaping for GUI text | as above |
| [utf8proc](https://github.com/JuliaStrings/utf8proc) | `03cb134` | MIT, plus the Unicode data licence for its tables | UTF-8 validation and grapheme segmentation | as above |
| [SheenBidi](https://github.com/Tehreer/SheenBidi) | `9c048a3` | Apache-2.0 | Unicode bidirectional resolution | as above |
| [libunibreak](https://github.com/adah1972/libunibreak) | `libunibreak_8_0` | Zlib | Unicode line-break opportunities | as above |
| [AccessKit C](https://github.com/AccessKit/accesskit-c) | `0.23.0` | MIT or Apache-2.0 | native assistive-technology adapters, built with Cargo | client and Studio |

---

## Fetched at configure or build time

Not in the repository; fetched from the network by a build step.

| Source | Licence | Fetched by | In a shipped game |
|---|---|---|---|
| shaderc `DEPS`: glslang, SPIRV-Tools, SPIRV-Headers | BSD-3-Clause / Apache-2.0 (glslang carries several licences in one file), Apache-2.0, MIT-style Khronos | `mono.vendor/shaderc/utils/git-sync-deps` (`just setup`, CI) into `mono.vendor/shaderc/third_party/` | with shaderc |
| shaderc `DEPS`: abseil-cpp, effcee, googletest, re2 | Apache-2.0, Apache-2.0, BSD-3-Clause, BSD-3-Clause | same | no; shaderc tests are off (`SHADERC_SKIP_TESTS`) |
| AccessKit's Rust crates (`mono.vendor/accesskit-c/Cargo.lock`) | mostly MIT or Apache-2.0; the crate licences are in Cargo's registry | `cargo` from `mono.build/MonoVendor.cmake` | statically linked into client and Studio |
| [Corrosion](https://github.com/corrosion-rs/corrosion) `v0.6.1` | MIT | `FetchContent` in `mono.vendor/accesskit-c/CMakeLists.txt`, only if that file is used directly | no; the engine build drives Cargo itself |
| [sentry-native](https://github.com/getsentry/sentry-native) `0.11.0` | MIT | `FetchContent` in luau-lsp when `LSP_BUILD_WITH_SENTRY=ON` | no; off by default and `just luau-lsp` does not enable it |
| [appimagetool](https://github.com/AppImage/appimagetool) `continuous` | MIT | `scripts/package-release.sh` on Linux unless `APPIMAGETOOL` is set | the AppImage runtime it embeds is in each `.AppImage` |
| [TypeScript](https://github.com/microsoft/TypeScript) `7.0.2` | Apache-2.0 | `package.json` `devDependencies`, via `bun install` or `npm install` | no; it compiles the TypeScript example scenes and runs `just typecheck` |

---

## Local patches to third-party code

A vendor is patched when `mono.vendor/patches/<name>/` holds a `.patch`. The
submodule is never written to: the pinned commit is `git archive`d into
`.cache/vendor/`, patched there, and the build uses the copy. Each patch file
opens with why it exists and when it stops applying.

| Patch | Vendor | What it does | Applied by |
|---|---|---|---|
| `sdl/vulkan-compute-samplers.patch` | SDL | Widens the sampler barrier stage mask to `ALL_COMMANDS` so compute-shader sampler reads are inside the dependency | `mono.build/MonoSdlQueues.cmake`, every client build |
| `sdl/vulkan-texture-cancellation.patch` | SDL | When a command buffer is cancelled, re-records the `UNDEFINED`-to-default layout transitions of freshly cycled texture backings, so they are not left `UNDEFINED` | same |
| `sdl/vulkan-defrag-source-layout.patch` | SDL | Returns the old backing from `COPY_SOURCE` to its default layout after a defragmentation copy, for commands that still pin it | same |
| `sdl/vulkan-queues.patch` | SDL | Project-private compute and transfer queue submissions on the same `VkDevice` (`ATOMIC_GPU_QUEUE_EXTENSION`, header `mono.build/private/AtomicGpuQueues.h`). Not for upstream | same, when `MONO_SDL_VULKAN_QUEUES=ON` (the default) |
| `tracy/defer-linux-kernel-symbols.patch` | Tracy | Defers the `/proc/kallsyms` scan from startup until a profiler connects | `scripts/vendor-tree.sh tracy` from `MonoVendor.cmake`, Linux with `MONO_TRACY` only |
| `tracy/rpmalloc-no-transparent-huge-pages.patch` | Tracy | `MADV_NOHUGEPAGE` on rpmalloc's queue regions, so an idle process does not keep about 8 MiB resident under `transparent_hugepage=always` | same |
| `imgui/before-input-callback.patch` | Dear ImGui | Adds an opt-in callback before text input is processed, reporting caret and selection, so Studio's script editor can own undo history | `mono.build/PreparePatchedImgui.cmake` into `.cache/vendor/imgui/objects/<key>`, client tier. `just patched-imgui-check` (`scripts/tests/patched_imgui_test.py`) tests the cache publication |
| `luau-lsp/dotted-enum-types.patch` | luau-lsp | Registers dotted type prefixes so `local face: Enum.NormalId` resolves in the editor, matching `mono.tools/scriptcheck` | `just luau-lsp` via `scripts/vendor-tree.sh luau-lsp` |
| `luau-lsp/call-hierarchy-function-name-copy.patch` | luau-lsp | Copies a named `FunctionName` instead of moving a temporary, so GCC 13 builds with upstream `-Wall -Werror` | same |
| `luau-lsp/codegen-null-logger-guards.patch` | luau-lsp (its bundled Luau) | Guards Luau CodeGen lowering output on a non-null logger, silencing GCC 13 `-Wnonnull` without disabling warnings | same |

Patched SDL, Tracy and ImGui ship in client and Studio binaries; MIT, Zlib and
BSD-3-Clause all permit modification. The luau-lsp build is local and never
redistributed.

Build-configuration choices that are not source patches (options forced in
`mono.build/MonoVendor.cmake`) are listed under each library below.

---

## What actually ships

- **Catch2 is test-only.** No test framework is in a client or server binary.
- **shaderc is two halves.** `glslc` compiles built-in shaders during the build
  and goes nowhere near a binary. `libshaderc` is for runtime shader compiles
  through `engine::render::ShaderCompiler`. `render` links it privately, but
  while nothing in the client calls the compiler the linker drops every
  object, which is why the table says "once a caller exists". Dead-stripping is
  per object file, not per function, and some libraries drag in a floor
  regardless, so "we only call one function" is never a reason to drop a
  notice. Check a release `client` with `nm`.
- **SPIRV-Cross is the build-time MSL cross-check.** The renderer feeds SPIR-V
  to Vulkan or MoltenVK; `mono.tools/shadercross` still translates built-in
  shaders during the build so an unsupported Metal construct fails early.
  Whether `Engine::msl`'s objects survive dead-stripping is a question for `nm`
  against a release build. Only the parser, GLSL and MSL emitters are built;
  HLSL, C++, reflection, C API and CLI are off in `MonoVendor.cmake`.
- **SDL3, MoltenVK, shaderc, SPIRV-Cross, Dear ImGui and AccessKit are
  client-tier.** The `server` preset does not configure them.
- **Tracy is compiled in but on demand.** It collects nothing until a profiler
  attaches, listens on localhost only, and does not broadcast.
- **Crypto++ ships in client and server.** `net`, `assets` and `bake` all pull
  archive members, so there is no tier split. BSL-1.0 exempts object code from
  attribution; a source distribution owes `License.txt` whole.
  `cryptopp-cmake` produces no object code and is listed because the rule is
  one entry per submodule.
- **BLAKE3 can be taken under CC0.** Only `mono.vendor/blake3/c/` is built; the
  Rust crate is not compiled. It ships wherever `assets` is linked, which is
  every program.
- **ngtcp2 is transport only.** Its crypto is a callback table filled in from
  `net/quic/Crypto.hpp` over Crypto++, so no TLS library is linked.
  `ENABLE_LIB_ONLY` is forced on: upstream's OpenSSL, GnuTLS, wolfSSL,
  picotls and BoringSSL helpers and its examples are never configured.
- **Zstandard is taken under BSD-3-Clause.** Upstream ships `LICENSE`
  (BSD-3-Clause) and `COPYING` (GPLv2); the GPLv2 option would be
  incompatible with shipping inside a game binary. Only the library is built
  (`ZSTD_BUILD_PROGRAMS` and `ZSTD_BUILD_TESTS` off), and legacy 0.x frame
  support is off.
- **utf8proc carries two notices.** Its MIT code and the Unicode data licence
  for the tables it compiles in; both are in its `LICENSE.md`.
- **AccessKit pulls a Rust dependency tree.** The static library Cargo builds
  contains every crate in `Cargo.lock` for the target platform; their licences
  are MIT, Apache-2.0 or both, and `cargo` records each in the registry copy.
  `LICENSE.chromium` in the submodule covers code AccessKit derives from
  Chromium.
- **doxygen-awesome-css is a stylesheet.** `just docs` copies it beside the
  generated HTML; it is never compiled, linked or staged.
- **Box2D and luau-lsp are not part of the build.** luau-lsp brings its own
  Luau and declares the same targets ours does, so `just luau-lsp` builds it in
  `.cache/build/luau-lsp`; `.gitmodules` has the reasoning.

---

## Fonts

`mono.vendor/fonts/` holds four unmodified variable `.ttf` files, each with the
OFL text upstream ships beside it, which is the whole obligation OFL 1.1 places
on a binary distribution. They are files rather than submodules because each
upstream repository is tens of megabytes of sources to obtain one font. Being
unmodified, the OFL reserved-name clause is not engaged. They are used at their
default (regular) instance.

| Font | Licence | Used for | In a shipped game |
|---|---|---|---|
| [Inter](https://github.com/rsms/inter) | SIL OFL 1.1 | interface text in the editor and game UI | staged beside programs linking `engine::ui` or `engine::render` |
| [JetBrains Mono](https://github.com/JetBrains/JetBrainsMono) | SIL OFL 1.1 | the script editor's monospace face | as above |
| [Roboto](https://github.com/googlefonts/roboto-classic) | SIL OFL 1.1 | the display face | as above |
| [Noto Sans](https://github.com/notofonts/latin-greek-cyrillic) | SIL OFL 1.1 | fallback coverage, loaded as a separate face | as above |

---

## Art and audio

**`mono.engine/render/src/DefaultTexture.inl` is content compiled in.** One
channel of ambientCG's *Plastic 013 A* colour map, box-filtered from 1024 to 64
and lifted so its brightest texel is white, as a C array. It is what every part
with no material draws as. **CC0-1.0**; recorded because provenance of a shipped
asset is recorded whether or not a licence requires it.

`scripts/fetch-materials.py` downloads material sets into `.cache/`, never into
the repository:

| Source | Licence | What comes from it |
|---|---|---|
| [ambientCG](https://ambientcg.com/) | CC0-1.0 | PBR material sets, and the compiled-in default above |
| [Poly Haven](https://polyhaven.com/) | CC0-1.0 | PBR material sets |
| [cgbookcase](https://www.cgbookcase.com/) | CC0-1.0 | PBR material sets |

**`mono.engine/examples/assets/audio/tornado-*.wav`** (five files) ship in
`client/audio/` and `studio/audio/` of every release archive. Their source and
licence are not recorded in the repository. Record them here before the next
release.

`assets/icon.png`, `assets/small-icon.png` and `assets/small-icon.ico` are the
project's own icons (the AppImage icon comes from `small-icon.png`), under the
repository's licence.

---

## Ported code

**The simplex kernels, analytical-derivative algebra and Worley cell search in
`mono.engine/examples/assets/scripts/libraries/TerrainCore/Noise.luau` were
ported from [Atlas](https://github.com/shmeatley/atlas) (MIT)**, a procedural
generation toolkit for Roblox. The file names it at the top and says which
parts.

| Source | Licence | What came from it | In a shipped game |
|---|---|---|---|
| [Atlas](https://github.com/shmeatley/atlas) | MIT | simplex, Worley and derivative kernels in the `TerrainCore` example library | only if a game ships that example |

Only the maths was kept: Atlas's samplers depend on Roblox types. Its shuffled
256-entry permutation table was deliberately not ported, because it repeats the
field every 256 units; the kernels use `Noise.luau`'s arithmetic hash instead,
and a test asserts the difference.

---

## If you add one

1. Check the licence is compatible with MPL-2.0 **before** anything else.
2. Add it as a submodule under `mono.vendor/`, pinned to a release branch or
   tag.
3. Add a row here with its version, its use and whether it reaches a shipped
   binary, and its notice to `mono.vendor/THIRD_PARTY_NOTICES.md`.
4. Configure it in `mono.build/MonoVendor.cmake`, not in the module that
   consumes it.
5. If it needs a source change, put the patch in `mono.vendor/patches/<name>/`
   and add it to the patch table above.

`mono.vendor/AGENTS.md` has the rest.
