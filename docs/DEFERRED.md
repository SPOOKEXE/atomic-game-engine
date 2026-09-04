# DEFERRED

Retired deferred items are in `docs/retired/DEFERRED.md`.

## Format

Each section has the header `[_] D00000`.

The numerical is a counter that increments every item. Insert the newest items
at the front of `## Deferred Items`; the highest number in this file and the
retired register determines the next number. Do not reuse a retired number.

An item that is closed or no longer exists is removed, not marked. Its decision
belongs in the closing commit and, when it affects scope, in `ROADMAP.md`.

```
### [_] D00101

- item 1
- item 2
- item 3
```

## Deferred Items

### [_] D00129

The remaining 2D interface gaps need capabilities the engine does not have.
Do not expose a property that can only answer a default.
Completed interface work is archived as D00130.

- **`VideoFrame`.** Needs a video decoder and `assets::VideoData` beside
  `TextureData`. `Playing`, `TimePosition`, `Looped`, and `Volume` are facts
  about a stream that does not exist.
- **`ImageLabel.IsLoaded` and `ImageButton.IsLoaded`.** Need a shared,
  per-content state table filled by the client texture cache. `gui` is shared
  and cannot depend on the client cache.
- **`TextLabel.OpenTypeFeatures` and `.TextDirection`.** Need HarfBuzz or an
  equivalent shaping layer behind `GlyphAtlas`.
- **`GuiObject.GuiState`.** Router hover and press state is local to one client
  and deliberately is not replicated. Exposing it as a component would make
  several clients write the same instance.
- **`UIStroke.ZIndex`.** Needs a keyed draw-list ordering, rather than the
  compiler's current paint sequence, so a stroke can sort independently of its
  element.
- **`SurfaceGui.ToolPunchThroughDistance`.** Needs a tool interaction ray that
  can cross the world and click a surface GUI.
- **Depth-tested `SelectionBox` adornments.** `AlwaysOnTop` is supported by
  the Studio overlay. The depth-tested case needs a world-space adornment pass.

`InputSink` duplicates `Active`. `BillboardGui` distance limits are deprecated
by the implemented `DistanceStep`. They are not deferred work.

### [_] D00119

`Player.CharacterAppearanceId` needs an avatar format, catalogue, and loader.
The current `CharacterDesc` is only colours and boxes, so an id would name
nothing. Reopen this when `MakeCharacter` can consume an authored avatar
description.
Completed team and spawn work is archived as D00131.

Team-creation permissions also wait for a collaborative-editing permission
model. There is no engine consumer to attach such a rule to today.

### [_] D00116

Clients cannot request a teleport. `TeleportService` remains server-only,
because moving a player is an authority decision and not a property write.

The transport shape exists, but the missing part is game policy: a host must
decide which player may request which destination. Reopen this for a game that
needs client-initiated travel and supplies that policy. The shared client
follow behaviour belongs beside `game::ApplyMoveInput`, not in a second host
implementation.
Completed replica script and GUI arrival work is archived as D00132.

### [_] D00109

`replication::Prediction` records and reconciles opaque inputs, but no client
simulation replays `Connector::Unconfirmed()`. That is deliberate: the current
character controller sends intent to the server and does not run a local physics
step, so it has no state to reconcile.

Reopen this only for an interaction whose latency requires a client simulation
step. The consumer must apply the remaining opaque inputs after reconciliation;
`Connector` cannot do that without knowing what each input means.
Completed input-path wiring is archived as D00133.

### [_] D00106

JavaScript and TypeScript breakpoints require a QuickJS debugger API. The
vendored QuickJS exposes an interrupt handler, but no file, line, frame, or
debugger interface. Source instrumentation would alter the program being
debugged and needs a complete JavaScript lexer, so it is not a small fallback.

Breakpoint requests for JavaScript-family chunks are refused with this reason.
TypeScript source maps already make stack traces name source lines correctly.
Reopen this for a vendored QuickJS debugger API, or a TypeScript plugin large
enough to make maintaining an alternative worthwhile.
Completed TypeScript source-map support is archived as D00134.

### [_] D00046

SDL has no portable GPU timestamp-query API. Vulkan timings use its native
bridge; Metal and Direct3D 12 remain unmeasured rather than displaying CPU
submission time as GPU cost.

Reopen this with Metal and Direct3D 12 test machines, or an SDL GPU timestamp
API that removes the backend bridges.

### [_] D00030

A mutable property on a Luau service global can be cached by `safeenv` when it
is read through a bare global. The idiomatic
`game:GetService("UserInputService")` local is not cached and is what the
engine's bindings, tests, and examples use.

Do not drop sandboxing or change the surface away from Roblox compatibility for
an access pattern no authored code uses. Reopen this if an authored script,
example, or panel demonstrates a stale bare-global read.

### [_] D00019

The engine's Luau revision must stay aligned with the revision consumed by
`luau-lsp`, so runtime behaviour and editor diagnostics agree. `just luau-lsp`
checks both pins and refuses drift.

Reopen this when upstream `luau-lsp` moves its Luau revision. Bump both
submodules together, run `just luau-lsp`, then run the normal validation suite.
The completed v0.15 synchronisation is archived as D00137.

### [PARTIAL] D00017

World lifetime is implemented in `world::DecideLifecycle` and shared by Studio
and the server. Placement remains open: deciding which host runs a world, and
what happens when that host dies, needs a deployment with more than one real
host. The `--worlds` harness and Studio's single-process host do not provide
that caller.
Completed lifetime work is archived as D00135.

### [_] D00001

The remaining macOS work requires a Mac: compile the generated MSL with
`metal`, run a client, verify SDL's Metal bindings and depth behaviour, and run
a runtime `ShaderScript` end to end. Linux validates the translation structure
and resource bindings but cannot make those platform claims.

Reopen this when somebody configures a client preset on Darwin.
Completed runtime, ECS, and shader-translation work is archived as D00136.
