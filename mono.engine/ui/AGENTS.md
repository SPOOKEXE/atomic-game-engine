# ui - module invariants

L12, `client` tier. The Dear ImGui integration, and the only target in the
repository that links it.

## This module is a boundary, not a layer

Nothing in `mono.engine` may depend on `ui`. The edge runs one way: `ui`
implements `render::FrameOverlayHook`, and `render` knows nothing about it.

The moment a simulation module wants "just one debug window from here", that
window has put an editor toolkit on a dedicated server's link line - and the
tier rule will not catch it, because `ui` is `client` and so is everything that
would reach for it. This is a rule the build cannot check, which is why it is
written here.

## The overlay panels are not obsolete and are not to be ported

`render::OverlayImage` draws F3, F4 and F5 by hand into a CPU buffer. That is
not legacy waiting for a rewrite. `Overlay.hpp` states the reason: those panels
have to work when the renderer is the thing being debugged, and an imgui panel
is drawn by the pipeline it is reporting on.

Porting them here would mean that a frame the renderer cannot present is a
frame whose profiler you cannot read, at exactly the moment you need it.

## No `ImGui::` call outside the frame bracket

`Interface::Begin` to `Interface::End`. imgui's own rule, restated because the
failure is not a compile error - it is a use of a context whose frame data was
already consumed, which crashes somewhere else entirely.

`Prepare` and `Record` are called by the renderer, after `End`, from inside its
command buffer. Do not call them yourself.

## `Prepare` uploads and `Record` draws, and they cannot be merged

SDL refuses to open a copy pass while a render pass is in flight. The imgui
backend uploads vertices through a copy pass. A hook that uploaded from
`Record` works until the first frame whose widget count grows a buffer, which
is months after the code that caused it.

## `Initialise` failing is not always an error

It needs a window, and a headless program does not have one. `mono.studio`
treats the refusal as fatal with a window and as expected without one. Do not
turn the refusal into an abort - a caller that legitimately has no display is
the case this module has to survive rather than the case it should stop.

## The interface holds no world state

Selection, expansion, scroll, splitter positions, which panels are open - none
of it goes in a store, crosses a bus, or enters a snapshot. Rule 2 is about
data another module also reads, and nobody replicates a scroll position.

The corollary is the one that bites: a panel must never keep its own copy of
something the store owns. Read it every frame. A cached instance name is a name
that is wrong for one frame after a rename, and one frame is enough to be seen.

## Colours live in `Theme.cpp`

Same rule as `input`'s `BINDINGS`. A literal colour at a point of use is a
colour that cannot be changed without finding every copy, and the copies drift.

## `ui` is Dear ImGui and `gui` will be the engine's own - they are not the same thing

v0.8 adds `mono.engine/gui`: the `GuiObject` tree a *game* builds its interface
out of, `shared` tier, saved into game files and replicated. This module is the
*editor's* toolkit, `client` tier, and nothing a game ships touches it.

They will coexist for a version and that is deliberate - the studio keeps Dear
ImGui until `gui` can draw a property grid, because an editor half on each is
two widget sets and the rule against two ways to do one job applies hardest to
the thing you look at all day.

**The one thing to share is the glyph atlas.** Four faces are vendored here and
`gui`'s text pass needs the same four files; a second rasteriser over them would
be two answers to what a glyph looks like. Whichever module ends up owning the
atlas, only one does.

## Fonts are asked for by role

`Typeface::Monospace`, not "JetBrains Mono". Swapping a family is a line in
`FAMILIES` and not a search - the same rule as `input`'s `BINDINGS` and this
module's palette.

## A pushed font must be popped inside the window that pushed it

`ScopedFont` exists so the pop cannot be forgotten, and it does not help if the
scope is wrong: a font still pushed when `ImGui::End` runs asserts as "Missing
PopFont()" at the *end of the frame*, naming neither the window nor the font.
Scope it to the run of text, not to the function.

## Studio chrome is a separate presentation layer

Dear ImGui produces the host interface only. Its signature may invalidate the
Studio composition and final image, but it must never invalidate a viewport's
scene image or the game's compiled interface. The game interface remains
`engine::gui::Compiled` plus the render interface pass; do not merge it into
this module to share a cache.

The host signature describes commands and resources, not backend texture
handles whose numeric value may change without visible pixels changing. When a
signature matches, reuse the resident host geometry and bindings. Cache hit and
write counts are recorded by the viewport's `PresentationDamageTracker`, not as
fake-duration spans inside this module.
