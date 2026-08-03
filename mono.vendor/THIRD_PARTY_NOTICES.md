# THIRD_PARTY_NOTICES

Third-party software distributed with, or linked into, binaries built from this
repository. One entry per submodule in `mono.vendor/`.

The engine itself is licensed under the Mozilla Public License 2.0 (`LICENSE`).
Nothing below changes that; each entry is a separate work under its own terms,
and those terms are reproduced here because every licence in this file requires
its notice to travel with the binary.

**This file is a distribution obligation, not bookkeeping.** MIT, BSD-3-Clause,
zlib and BSL-1.0 all require the copyright notice and the permission text to
appear in distributions of the software, including binary-only ones. Shipping a
client without carrying these notices breaches every one of them. See §3 for
what a shipped build has to contain.

Adding a submodule means adding an entry here — step 3 of "Adding one" in
`AGENTS.md`. Removing one means removing its entry, which matters just as much:
a notice for something no longer shipped is a claim that is not true.

Exact versions are pinned by the superproject commit, not by this file. Where a
tracking branch is recorded in `.gitmodules` it is named below, because that is
what a fresh `just setup` follows.

---

## 1 · What ships where

Tier decides which binaries a dependency reaches, and therefore which
distributions must carry its notice. Configuration lives in
`mono.build/MonoVendor.cmake`; this table is the summary.

| Library | In client | In server | In tests only | Licence |
|---|---|---|---|---|
| SDL | yes | **no** | — | zlib |
| glm | yes | yes | — | Happy Bunny **or** MIT |
| spdlog (bundles fmt) | yes | yes | — | MIT (fmt: MIT) |
| Tracy | when `MONO_TRACY` | when `MONO_TRACY` | — | BSD-3-Clause |
| shaderc | yes | **no** | — | Apache-2.0 |
| ├ glslang | yes | **no** | — | six licences — §2 |
| ├ SPIRV-Tools | yes | **no** | — | Apache-2.0 |
| └ SPIRV-Headers | yes | **no** | — | MIT-style Khronos |
| Catch2 | — | — | yes | BSL-1.0 |
| Crypto++ | yes | yes | — | BSL-1.0 (files public domain) |
| cryptopp-cmake | **no** | **no** | **no** | BSD-3-Clause |
| BLAKE3 | once `assets` is linked | once `assets` is linked | — | CC0-1.0 **or** Apache-2.0 **or** Apache-2.0-LLVM |
| Zstandard | once `cdn` links it | once `cdn` links it | — | BSD-3-Clause **or** GPLv2 — **we take BSD** |

shaderc is one submodule and **four** notices: it pins glslang, SPIRV-Tools and
SPIRV-Headers in its own `DEPS` file rather than as submodules, so they arrive
under `mono.vendor/shaderc/third_party/` and are easy to miss when reading
`.gitmodules` alone. All four link into the client and none into the server, the
same tier split as SDL.

SDL being client-only is a real distribution difference, not a technicality: a
server build links no SDL, so a server-only distribution does not need to carry
its notice. That is a consequence of the tier rule in `AGENTS.md` — "who may
link what is a tier question" — and it is the clearest example of why that rule
is worth enforcing.

Catch2 is test-only. It is not in any shipped artefact, so no shipped artefact
needs its notice. It is listed because it is a submodule here and the rule is
one entry per submodule.

---

## 2 · Notices

### SDL — zlib licence

- Upstream: https://github.com/libsdl-org/SDL — branch `release-3.2.x`
- Full text: `mono.vendor/sdl/LICENSE.txt`

```
Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

Linked as a shared library, so the SDL binary is redistributed alongside the
engine rather than absorbed into it. Restriction 2 is the one to watch: if this
project ever patches SDL rather than upstreaming the fix, the altered source has
to be marked. `AGENTS.md` already forbids local patches for a different reason,
which happens to keep this clause satisfied by construction.

### flecs — removed at v0.2, submodule deleted at v0.4

flecs was the ECS storage for v0.1 and is no longer built, linked or shipped.
`engine::ecs` owns its storage outright — `Column`, `ComponentSet`, `SparseSet`
and the archetype behind them — which is what `Store::Native()` existed to
apologise for and what its removal settles.

**The submodule is gone.** This entry used to end "delete it when convenient",
and it stayed checked out for two versions after nothing configured it — which
is the small version of the thing this repository keeps finding: **an artefact
nothing builds is one nobody re-reads, and it goes on looking like a dependency
to everyone who greps for one.** It was still costing every `just setup` a clone
and still appearing in a licence count that the table below it had already
stopped listing.

Kept as an entry rather than deleted, because "why is there no flecs" is a
question the v0.1 history invites and the answer belongs somewhere.

### glm — The Happy Bunny License or MIT

- Upstream: https://github.com/g-truc/glm — branch `master`
- Full text: `mono.vendor/glm/copying.txt` (both licences, in full)

```
Copyright (c) 2005 - G-Truc Creation
```

Dual-licensed: a user may choose either. **This project takes the MIT option.**
Worth stating rather than leaving implicit — the Happy Bunny License is MIT plus
a clause asking that the software not be used for military purposes, which is a
term some downstream distributors will not accept. Taking MIT keeps the
obligation to the ordinary notice-retention one.

`VENDOR_PUBLIC` — glm types appear in `core/types/CFrame.hpp`.

### spdlog — MIT, and it bundles fmt

- Upstream: https://github.com/gabime/spdlog — branch `v1.x`
- Full text: `mono.vendor/spdlog/LICENSE`

```
Copyright (c) 2016 - present, Gabi Melman and spdlog contributors.
```

**spdlog vendors fmt inside itself**, at
`mono.vendor/spdlog/include/spdlog/fmt/bundled/`. It is a separate work with its
own notice and is easy to miss, because it arrives as a subdirectory of a
submodule rather than as a submodule:

```
Formatting library for C++
Copyright (c) 2012 - present, Victor Zverovich
```

fmt is MIT-licensed. Both notices have to ship; carrying only spdlog's would be
incomplete.

`VENDOR_PUBLIC` — spdlog appears in `core/Log.hpp`. Note that only the *sink* is
spdlog; `log` as a userland library is first-party, per
`DATATYPES_LIBRARIES.md`.

### Tracy — BSD-3-Clause

- Upstream: https://github.com/wolfpld/tracy — branch `master`
- Full text: `mono.vendor/tracy/LICENSE`

```
Tracy Profiler (https://github.com/wolfpld/tracy) is licensed under the
3-clause BSD license.

Copyright (c) 2017-2026, Bartosz Taudul <wolf@nereid.pl>
All rights reserved.
```

BSD-3-Clause: redistribution in source or binary form, with or without
modification, is permitted provided the copyright notice, the condition list and
the disclaimer are retained; and provided that neither the name of the copyright
holder nor the names of contributors are used to endorse or promote derived
products without prior written permission.

That third clause is the one with teeth here: **do not use Tracy's name or
Bartosz Taudul's to promote this engine.** Describing the profiler integration
factually in documentation is fine; "powered by Tracy" on a product page is the
kind of claim the clause exists to prevent without written permission.

The client is compiled in for ordinary builds (`TRACY_ON_DEMAND`), so this
notice ships whenever `MONO_TRACY` is on — which is the default. It is not a
tools-only dependency.

### Catch2 — Boost Software License 1.0

- Upstream: https://github.com/catchorg/Catch2 — branch `devel`
- Full text: `mono.vendor/catch2/LICENSE.txt`

BSL-1.0 permits use, reproduction, display, distribution, execution and
transmission, and the preparation of derivative works, provided the copyright
notices and the licence statement are included in all copies of the software in
source form. **Binary distributions carry no attribution requirement** — the
licence explicitly excludes machine-executable object code from that condition,
which is why BSL-1.0 is commonly chosen for test frameworks.

Test-only in this repository, so it reaches no shipped artefact regardless.

---

### shaderc — Apache-2.0

- Upstream: https://github.com/google/shaderc — branch `main`
- Full text: `mono.vendor/shaderc/LICENSE`

```
Copyright The Shaderc Authors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
```

**Apache-2.0 asks for more than the other licences here**, and the two clauses
that matter are §4(d) and §3:

- §4(d) requires the contents of a `NOTICE` file to be reproduced, *if the work
  carries one*. Checked: **shaderc ships no `NOTICE` file**, and neither does
  SPIRV-Tools. So there is nothing extra to carry beyond the licence text and
  the attribution below. Re-check this on a version bump; it is the kind of file
  that appears without anyone noticing.
- §3 is an express patent grant that terminates for anyone who initiates patent
  litigation over the work. It costs nothing to comply with and is worth knowing
  is there.

Linked only into client-tier binaries. `glslc`, the command-line driver from the
same project that the build finds with `find_program`, is a build-time tool and
is not redistributed.

### glslang — six licences in one file

- Upstream: pinned by shaderc's `DEPS`, at `mono.vendor/shaderc/third_party/glslang`
- Full text: `mono.vendor/shaderc/third_party/glslang/LICENSE.txt` — **1013 lines,
  reproduce it whole**

`LICENSE.txt` concatenates, in order:

1. 3-Clause BSD — the core of glslang-proper, minus the preprocessor
2. 2-Clause BSD — Copyright 2020 The Khronos Group Inc
3. The MIT License — Copyright 2020 The Khronos Group Inc
4. Apache-2.0
5. **GPL-3, with the special Bison exception**
6. An NVIDIA notice, whose operative clause is non-endorsement

Any one-line summary of this is wrong, including "BSD". Ship the file.

**On item 5, because it will stop a licence review.** Checked against the
vendored revision rather than inferred:

- Exactly **two** files in the whole of glslang mention the GNU GPL:
  `glslang/MachineIndependent/glslang_tab.cpp` and its `.cpp.h`. Everything else
  is BSD, MIT or Apache.
- Both are Bison output — *"A Bison parser, made by GNU Bison 3.8.2"*,
  Copyright (C) 1984‑2021 Free Software Foundation.
- Both **are compiled in**: `glslang/CMakeLists.txt` lines 64 and 96.
- Both carry the special Bison exception in full, which permits creating a
  larger work containing part or all of the parser skeleton and distributing it
  **under terms of your choice**, on one condition — that the larger work is not
  *itself a parser generator* using the skeleton as a parser skeleton.

A game engine is not a parser generator, so the condition is met and no copyleft
obligation reaches this repository. That is the ordinary situation for Bison
output, which is why it sits inside a great deal of proprietary software.

**Do not repeat glslang's own explanation for this.** Its `LICENSE.txt` header
says *"Bison was removed long ago. You can build glslang from the source
grammar, using tools of your choice, without using bison or any bison files."*
That is **not true of the vendored tree**: `glslang.y`, `glslang_tab.cpp` and
`glslang_tab.cpp.h` are all present and the generated pair is built. The reason
this is fine is **the exception**, not the removal — an argument resting on the
removal claim collapses the moment anyone runs `find`.

Re-run that check on a version bump. It is four commands and it is the only part
of this file where a wrong answer is expensive.

### SPIRV-Tools — Apache-2.0

- Upstream: pinned by shaderc's `DEPS`, at `mono.vendor/shaderc/third_party/spirv-tools`
- Full text: `mono.vendor/shaderc/third_party/spirv-tools/LICENSE`

```
Copyright (c) 2015-2016 The Khronos Group Inc.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0
```

Same Apache-2.0 notes as shaderc above. No `NOTICE` file at the pinned revision.

### SPIRV-Headers — MIT-style Khronos licence

- Upstream: pinned by shaderc's `DEPS`, at `mono.vendor/shaderc/third_party/spirv-headers`
- Full text: `mono.vendor/shaderc/third_party/spirv-headers/LICENSE`

```
Files: All files except for those called out below.
Copyright (c) 2015-2024 The Khronos Group Inc.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and/or associated documentation files (the
"Materials"), to deal in the Materials without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Materials, and to
permit persons to whom the Materials are furnished to do so, subject to
the above copyright notice and this permission notice being included in
all copies or substantial portions of the Materials.
```

Note the *"except for those called out below"* — the file carves out a handful of
files under different terms. Another reason to ship the licence file rather than
a summary of it.

Header-only. It reaches the binary as compiled constants rather than as code,
which changes nothing about the attribution requirement.

### Crypto++ — Boost Software License 1.0, over public-domain files

- Upstream: https://github.com/weidai11/cryptopp — branch `master`
- Full text: `mono.vendor/cryptopp/License.txt` — **three licences, ship it whole**

```
Compilation Copyright (c) 1995-2019 by Wei Dai.  All rights reserved.
This copyright applies only to this software distribution package
as a compilation, and does not imply a copyright on any particular
file in the package.

All individual files in this compilation are placed in the public domain by
Wei Dai and other contributors.
```

**The licensing here is stronger than the one-word answer suggests, in our
favour.** The compilation is BSL-1.0; the individual files are public domain.
BSL-1.0 is therefore a floor rather than a ceiling, and it is the same licence
Catch2 and asio already carry — so this introduces no new licence class to the
repository and no new obligation type.

BSL-1.0's only real condition is that the notice travel with **source**
distributions. It explicitly exempts machine-executable object code, so a
shipped client or server carries no Crypto++ attribution requirement at all.
That is the same exemption noted under Catch2 above.

**Two further licences are bundled, and a summary of "BSL-1.0" misses both:**

- **CRYPTOGAMS**, Andy Polyakov's BSD-style licence, covering the 32-bit ARM
  assembly `aes_armv4.S`, `sha1_armv4.S` and `sha256_armv4.S`. Its operative
  clauses are notice retention and non-endorsement — the same shape as Tracy's
  third clause, and the same caution applies: do not use the CRYPTOGAMS name or
  Andy Polyakov's to promote this engine. Reachable only on 32-bit ARM targets;
  `License.txt` notes the code can be disabled through `config_asm.h` if a
  distribution would rather not carry the notice at all.
- **Botan**, for the ChaCha SSE2 and AVX implementations. Jack Lloyd placed that
  code in the public domain specifically for Crypto++ to use, so it adds no
  obligation — it is recorded because "why is there Botan code in here" is a
  question a licence review will ask.

Neither is a submodule, a `DEPS` entry, or a subdirectory. Both arrive as
ordinary files inside the tree, which is the same way fmt hides inside spdlog
and the reason this file lists notices rather than repositories.

**MPL-2.0 compatibility.** Nothing above requires the larger work to be
relicensed, and public-domain files cannot. This satisfies step 1 of "Adding
one" in `AGENTS.md`.

**Linked by `Engine::core`, and therefore in every binary this repository
builds.** `engine::core::Random` needs a value that is identical on every
machine rather than merely random, which is SHA-256's job here; `Tool::testrunner`
uses it again for the cascading test signature.

Because `core` is the module everything links, Crypto++ produces no tier split
the way SDL does — there is no configuration of this engine that ships it to the
client and not the server. Measured, the same way the shaderc claim above was:
**9,479 `CryptoPP::` symbols in `client` and 9,479 in `server`.** The counts are
identical because both reach it through the same module.

`docs/CPP_LINKER.md` §2 has what that costs and why it is not smaller: 36 of the
archive's 173 members, which is far more than SHA-256 needs — `cryptlib.o` is
unavoidable and drags the algorithm registry in behind it — and still under a
fifth of the library.

### cryptopp-cmake — BSD-3-Clause, and it ships nothing

- Upstream: https://github.com/abdes/cryptopp-cmake — branch `master`
- Full text: `mono.vendor/cryptopp-cmake/LICENSE`

```
BSD 3-Clause License

Copyright (c) 2018, The Authors.
All rights reserved.
```

BSD-3-Clause on the same terms set out under Tracy: retain the notice, the
condition list and the disclaimer; do not use the copyright holder's or
contributors' names to endorse or promote derived products without written
permission.

### BLAKE3 — CC0-1.0, or Apache-2.0, or Apache-2.0 with the LLVM exception

- Upstream: https://github.com/BLAKE3-team/BLAKE3 — branch `master`
- Full text: `mono.vendor/blake3/LICENSE_CC0`, `LICENSE_A2`, `LICENSE_A2LLVM`

```
This work is released into the public domain with CC0 1.0.
Alternatively, it is licensed under the Apache License 2.0, or the
Apache License (Version 2.0) with LLVM Exceptions.
```

**Three licences offered, and the choice is ours.** Taking the CC0 option makes
this a public-domain dedication with no attribution requirement in any
distribution, source or binary — the lightest obligation of anything in this
file, lighter even than Crypto++'s BSL-1.0. Nothing has to travel.

It is listed anyway. The rule here is one entry per submodule, and a licence
review that finds a submodule this file does not mention stops there and asks
why, regardless of what the licence turns out to permit.

**Only `blake3/c/` is built.** The repository also contains the Rust reference
implementation, `b3sum`, benchmarks and test vectors; none of them is compiled,
linked or staged. `mono.build/MonoVendor.cmake` adds the `c/` directory alone,
and `BLAKE3_USE_TBB` is forced off so that a configure never reaches GitHub for
oneTBB.

Where it ships is a consequence of who links `assets`, and `repo_layout.md` §8
answers that with every program — client, server, studio, CLI and the content
origin. So, like Crypto++ and unlike SDL, it produces no tier split: there is no
build of this engine that carries it for one program and not another.

### Zstandard — BSD-3-Clause, and that is a choice

- Upstream: https://github.com/facebook/zstd — branch `release`, v1.5.7
- Full text: `mono.vendor/zstd/LICENSE` (BSD-3-Clause)
- Also present: `mono.vendor/zstd/COPYING` (GPLv2) — **the option we do not take**

```
BSD License

For Zstandard software

Copyright (c) Meta Platforms, Inc. and affiliates. All rights reserved.
```

**Two licence files in one submodule, and only one of them applies to us.**
Zstandard is offered under BSD-3-Clause *or* GPLv2 and the licensee chooses. We
choose BSD-3-Clause. This is the one entry in this file where reading the wrong
text leads to a materially wrong conclusion — GPLv2 would be incompatible with
shipping this inside a game binary under MPL-2.0, so the choice is recorded here
rather than left to be inferred.

Obligations are the ordinary BSD-3-Clause ones set out under Tracy: retain the
notice, the condition list and the disclaimer; do not use the copyright holder's
name to endorse derived products.

**Only the library is built.** `ZSTD_BUILD_PROGRAMS` and `ZSTD_BUILD_TESTS` are
forced off in `MonoVendor.cmake`, so the `zstd` command-line tool is neither
compiled nor shipped. `ZSTD_LEGACY_SUPPORT` is off as well, and that is a
security decision as much as a size one: it decodes 0.x frames that nothing here
could ever have written, and it is decoder surface parsing bytes an origin
supplied.

**This is a build system, not a library.** It exists because `weidai11/cryptopp`
ships a GNUmakefile and no CMakeLists at all, and because reimplementing that
makefile here — 202 translation units, a per-file ISA flag matrix across x86,
ARM and POWER, a static-initialisation-ordered link — would be first-party build
code shadowing upstream's, which is the drift `AGENTS.md` exists to prevent.

It produces no object code. **No binary can contain it, so no distribution needs
its notice.** It is listed anyway for two reasons: the rule here is one entry per
submodule, and an unlisted BSD-3-Clause submodule is exactly the kind of thing
that stops a licence review halfway.

**The two are version-coupled.** `cryptopp/sources.cmake` carries an explicit
source list rather than a glob, so a Crypto++ bump that adds or renames a `.cpp`
silently drops that file from the library unless cryptopp-cmake moves with it.
Verified at the pinned revisions — every `.cpp` in `mono.vendor/cryptopp` appears
in the list, and the only extras are the optional PEM Pack (`pem_*.cpp`,
`x509cert.cpp`, off unless `CRYPTOPP_USE_PEM_PACK`) and `adhoc.cpp`, which is
commented out. Re-run that comparison on a bump; it is two `comm` invocations:

```sh
grep -oE '[a-z0-9_]+\.cpp' mono.vendor/cryptopp-cmake/cryptopp/sources.cmake | sort -u > /tmp/want
ls mono.vendor/cryptopp/*.cpp | xargs -n1 basename | sort -u > /tmp/have
comm -13 /tmp/want /tmp/have   # sources present but unbuilt — must be empty
```

---

## 3 · What a shipped build must carry

A **client** distribution has to include the notices for SDL, glm, spdlog
(with fmt), Crypto++, shaderc, glslang, SPIRV-Tools, SPIRV-Headers, and Tracy
when it is compiled in. glslang's `LICENSE.txt` goes in whole, all 1013 lines of
it.

A **server** distribution has to include glm, spdlog (with fmt), Crypto++
and Tracy — and **not** SDL, and **not** the four shaderc projects. Both are
client-tier.

**Crypto++ is in both lists.** `Engine::core` links it for
`engine::core::Random`, and everything links `core`, so it is portable C++ with
no tier split — unlike SDL there is no build that carries it for one and not the
other.

Its obligation is the lightest here even so: BSL-1.0 exempts machine-executable
object code from attribution, so a **binary-only** distribution owes nothing at
all. A **source** distribution owes `License.txt` in full — all three licences
in it, CRYPTOGAMS and the Botan note included. cryptopp-cmake is never owed by
any distribution; it compiles to nothing.

The mechanism is deliberately not decided here. Two options, both ordinary:

- Stage this file next to the binary, alongside the engine's own `LICENSE`.
  Simplest, and slightly over-inclusive for a server build.
- Generate a per-tier notices file at install time from the table in §1. Exact,
  and one more thing that can silently go stale.

Either satisfies the obligation. Doing neither does not.

**This file is not legal advice, and it is not a substitute for reading the
licences it points at.** It is an inventory with the notice text that has to
travel, assembled so that the question "what do we owe, and to whom" has an
answer that does not require walking the submodule tree.

---
