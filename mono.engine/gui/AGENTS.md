# gui - module invariants

L7, `shared` tier. What a 2D thing in a *game* is: the class tree, the layout
arithmetic, the compiled draw list and the input routing. `scene` answers the
same question for a part, and most of what follows is that module's argument
one dimension down.

## This is not `mono.engine/ui`, and the two must not converge

`ui` is Dear ImGui at L12 `client` - the **editor's** toolkit, the only target
that links imgui, and nothing a game ships touches it. This is the widget set a
game builds its own interface out of, and it is `shared` because a server
authors a `ScreenGui` and replicates it.

They coexist for a version deliberately. `ui/AGENTS.md` gives the reason: the
studio keeps Dear ImGui until this tree can draw a property grid, because an
editor half on each is two widget sets and the rule against two ways to do one
job applies hardest to the thing you look at all day.

**The one thing they share is the glyph atlas**, and the edge that carries it
runs `ui` → `gui` and never back. `ui::PaintGui` takes a `gui::DrawList` and an
`ImDrawList` and nothing else - no store, no class table, no tree. A reviewer
should refuse any include of an `engine/ui/` header from this module, and the
tier check will not catch it, because a `shared` module including a `client`
one fails at the link and not at the include.

## `shared`, and the tier check only half covers it

A `shared` module may not link a `client` one, so the build catches
`Engine::render` or `Engine::ui` on the link line. What it cannot catch is the
*shape*: a font handle, a texture id, a packed RGBA8 colour, a field sized to
match a vertex layout. Every one of those compiles and passes the tier check.

The test to apply is `scene/AGENTS.md`'s: **could a server with no graphics
stack installed produce this value, and mean it?** A `Rect` in canvas pixels
yes; an `ImTextureID` no. `DrawCommand::Image` is a `core::Name` for exactly
that reason, and resolving it to a texture is the backend's job.

## What it deliberately does not depend on, and cannot be made to

`core` and `ecs` are the whole dependency list. Both refusals below are
`shared`-to-`shared`, so **the build cannot catch either** - by rule 6 that
makes them conventions, and this is where they are written down.

- **`scene`.** It looks necessary twice and is not. `SurfaceGui::Face` wants
  `NormalId`, which is a six-member enum registered by name - `gui::Face`
  declares the same six in the same order and `gui/tests/Enums.cpp` pins them,
  which is the arrangement `DefaultMaterial`'s "Plastic" already uses. And a
  surface gui's canvas wants the adornee's stud extent, which is `D00022`: it
  belongs to whoever draws one and has both operands, not here.
- **`world`.** Legal by height and still wrong, for the reason `scene` refuses
  it: a component set is data, and a data module that knows about universes and
  ticking is one the layout tests cannot use without standing a world up.
  `Layout` and `Compiled::Rebuild` take an `ecs::Store &` and nothing larger.

## The derived components have one writer each, and nothing keeps a second copy

`Resolved` is the first and the rule is its: one pass writes it, parent before
child. The draw list, the hit test and a script asking `AbsoluteSize` all read
that component with a query.

**`ScrollState` is the second and is the same rule applied to one class.** The
canvas extent after `AutomaticCanvasSize`, the window after the bars are inset
and the two thumb rectangles are all things `ContentArea` works out to place a
scrolling frame's children - and three consumers need them: a script reading
`AbsoluteCanvasSize`, the compile drawing the bars, and the router hit-testing a
drag. Before v0.18 the compile worked the thumbs out again, which is why the
bars could be seen and not grabbed. **A bar's geometry is decided once, in the
layout.**

A cached rectangle anywhere else is the stale-cache bug `scene::Bounds` refuses
in as many words, and here it has a specific failure: the compiled list and the
hit test would disagree about where a button is, which reads as "clicking is
off by a bit" and is close to undebuggable from the outside.

**`Rendered` is cleared by a sweep and never maintained at the write.**
Ancestry is not local - reparenting one frame moves everything beneath it - and
the alternative is hooking `SetParent`, the explorer's drag, the loader and
`DestroyInstance`, where missing one gives an element that draws after being
detached. `scene::Visibility` reached the same conclusion for the same reason.

## The signature covers exactly what the compile reads

`Compile.hpp` carries the table. The rule it states is the load-bearing one: a
field added to a component has to be added to the fold in `Compile.cpp`, and
the failure if it is not is a UI one edit stale - a panel that updates on the
*next* unrelated change.

That is a rule the build cannot check, so `gui/tests/Compile.cpp` checks it:
it walks every property the class tree declares, writes each one, and asserts
the signature moved. **A property added without a fold fails that test.** Do
not weaken it by listing property names in the test - the walk is what makes it
cover things nobody thought to list.

The direction matters and only one way round is safe. A collision keeps a list
the world has moved on from; a spurious change costs one rebuild nobody sees.
Lean the second way, always.

## The game interface is its own retained cache layer

`Compiled` is the resident game-interface source for the presentation cache. It
keeps its draw list when its signature matches, and a caller must use the
`false` return from `Rebuild` to avoid uploading identical vertices. Scene
motion does not enter this signature and a game-interface edit does not dirty
the scene source. They meet only in the renderer's game composition.

`CompileRequest::ScreenGuis` is part of the signature and is a viewing policy,
not authored state. A shipped client selects `PlayerGui`. Studio selects
`StarterGui` while editing and `PlayerGui` while a world is running. The
`PlayerGui` choice also requires `CompileRequest::Viewer` and admits only that
player's subtree; a null viewer admits none. The generic default admits both for
tests and tools that deliberately inspect a whole store. Never make a
production caller rely on that default: rendering a template and live copies
together duplicates or leaks another player's interface and makes their cache
signatures move for unrelated edits.

The cache profiler belongs above this shared module. This module already
reports `Requests` and `Rebuilds`, while render records whether the retained
interface and its compositions were hits or writes. Do not add timing spans for
hits; a skipped compile has no useful elapsed region.

## Enums are stored as ordinals, so their order is the save format

Every enum in `Enums.hpp` sits in a trivially-copied component as its ordinal.
Reordering one loads cleanly and lays everything out somewhere plausible, and
nothing at load time could catch it.

`gui/tests/Enums.cpp` pins every member of every set, in order. Two of them are
worth knowing about before touching anything:

- `TextYAlignment` is `Top, Center, Bottom` and `TextXAlignment` is
  `Left, Center, Right`. Not the same order, deliberately - Roblox's numbering.
- `ScrollingDirection` starts at **one**: it is a bit pair, not a counter.
  `Classes.cpp`'s `EnumOrigin` applies the offset in both directions.

## The class tree is Roblox's, including the parts that look wrong

`GuiBase` and `GuiBase2d` add no components. Do not collapse them: `GuiBase3d`
and the adornments hang off `GuiBase` when they arrive, and a flattened tree
would have to grow the split back at exactly the point somebody is adding a
feature.

**A `LayerCollector` is not a `GuiObject`.** That is the relation everybody
assumes and Roblox does not have. A `ScreenGui` has no `Position` and no
`BackgroundColor3`; giving it `Element` would make the layout resolve a `UDim2`
against a parent it does not have.

**`Text` is declared on three classes rather than on a shared base**, because
there is no base to put it on - `TextButton` derives from `GuiButton`,
`TextLabel` from `GuiLabel`, `TextBox` from `GuiObject`. A synthetic
"TextThing" base would be a class no script has heard of appearing in `:IsA`
and in the bindings manifest, which is the worse trade.

## Layout is one pass and must stay one

A `UDim2` needs a parent rectangle, so resolving one is inherently top-down;
that is the single place the tree's shape is unavoidable, and everything
downstream wants a flat list.

Two things keep it to one pass and both are easy to break:

- **`Element::Rotation` does not affect layout.** A rotated child whose
  bounding box fed back into a list layout would need the layout to run twice
  to settle. Roblox does the same.
- **`Measure` is separate from `Place`** only because a list or a grid has to
  know how big a child is before deciding where it goes. Do not let `Measure`
  grow side effects; it is called speculatively.

`AutomaticSize` is the one property that genuinely needs a second phase, which
is why it is declared and not implemented. `D00021` carries the argument.

## Text is measured with a constant, and there is exactly one answer

`AVERAGE_ADVANCE` in `Layout.hpp` is a fraction of an em, and `TextScaled`
fits against it. The exact answer needs a glyph atlas, which is `client` and
cannot be here.

**A backend must draw at `Resolved::TextSize` rather than measuring again.**
Not because the estimate is good - because a backend with real metrics would
disagree with the hit test and with what a headless test asserts, and two
answers is the failure this module is arranged to avoid everywhere else.

## A backend lays text out, and that is why markup is spans

`RichText` parses to a plain string plus a list of **byte ranges** over it -
`DrawSpan` - and never to positioned runs. The reason is the section on
`AVERAGE_ADVANCE` above: only a backend can measure a glyph, so a compile that
placed the second word would place it with an estimate the renderer disagrees
with, and the emphasis would visibly drift as a panel resized. One string, one
layout pass, styles looked up per byte.

**A malformed string is shown literally, tags and all.** That is Roblox's
behaviour and the useful one: an author who typed `a < b` sees `a < b`, and one
who mistyped a tag sees the tag rather than a gap where their text was. Half a
parse is the outcome refused - a reader given a partly-stripped string cannot
tell whether the markup ran out or the text did.

**A gradient is the same shape one dimension over.** `gui::Gradient` authors an
angle and a size-relative offset; `Compile` resolves those into the two ends of
a line in canvas pixels, because everything a backend receives is in canvas
pixels. Two backends resolving an angle would be two answers to where a ramp
starts.

## Input produces events; it does not fire signals

`script::Signals` is L9 and this is L7, so nothing here can fire a
`.Activated`. `Router::Update` returns a list of `GuiEvent` and whoever owns
the scripting layer turns each into a signal.

That is not a workaround. An editor driving a UI with no scripts running wants
the hit testing and none of the dispatch, and a test wants to assert on the
events rather than on what a Luau handler did with them.

Two rules inside it are worth keeping:

- **`InputEnded` goes to where the press began, not where the release
  happened.** That is what makes dragging off a button and back one
  interaction. A release routed by position leaves the pressed button stuck
  looking pressed.

**Two gestures are not presses and must not become them**, and both arrived at
v0.18. A scroll bar's thumb and a `UIDragDetector` are grabbed *before* the
pick, because neither element is `Active` and the pick would walk straight past
both; and once one is held, the press path is skipped entirely. A drag that
emitted `InputBegan` would make every draggable button fire `Activated` each
time somebody moved it - which is a bug people file against the button.

**What a drag produces is `Element::Position`, which is authored and does
cross.** Where the gesture *is* - the grab point, the position it started from -
is the router's, exactly as the hover and the press are. A `UIDragDetector`
carrying a "being dragged" flag would be one replicated row two clients write.
- **`MouseLeave` fires before `MouseEnter`.** A handler that moves something on
  leave has to run before the one reacting to the arrival, or a swap between
  two adjacent buttons produces an enter against state the leave is about to
  undo.

## The keyboard's focus is the world's; the router only decides it

`Router` holds the hover and the press privately, because nobody else reads
where a mouse is. The focused `TextBox` is the opposite case and lives in
`GuiServiceState::FocusedTextBox`: the scripting layer reads it for
`UserInputService:GetFocusedTextBox` and L9 has no route to a router, so a
handle kept beside the decision would be rule 2's second statement of one fact.

That is why `Router::Update` takes a **mutable** `ecs::Store &`, and it is the
only thing in the class that writes one. A world with no `GuiService` routes the
pointer exactly as it did before focus existed and takes none at all - the
honest answer rather than an oversight, since there is nowhere for the fact to
live.

Three rules inside it, none of which the build can check:

- **Only a press moves it.** A hover does not take the keyboard and a release
  does not give it back, which is what makes dragging a selection out of a box
  and letting go somewhere else keep it focused - `InputEnded`'s rule one
  section up, for the same interaction.
- **A press that lands on nothing releases it**, which is Roblox's answer:
  clicking the background is how anybody stops typing, and the alternative
  leaves a game with no way to give the keyboard back to itself.
- **The handle is validated on the way out, never swept.** `FocusedTextBox`
  answers null for a box that has been destroyed, because the generation in the
  id has moved - the same argument `Rendered` is swept for, one section up.
  Reparenting therefore does *not* release the focus, deliberately. And a
  destroyed box emits no `FocusReleased`: the event names an element, and
  `Router::Forget` already states that firing at one that is gone is worse than
  firing nothing.

**A host has to install the services, and nothing in `gui` can do it for one.**
`InstallGuiServices` is where `GuiService` comes from, `scene::InstallServices`
cannot call it, and every function above answers "no" in a world that has
neither. Three hosts call it: `examples::LoadScene` for every `--script` world
in the client and the server, `studio::Editor::PrepareWorld` for every world the
editor makes, and `client::BuildReplicatedWorld` for a replica.

**A replica is completed rather than furnished, and that is what the third
caller needs.** No `gui.` component is replicated, so a client is shown a
`GuiService` that is a name and a class with no state on it - and `Select` and
`Focus` both read that state and both answer `false` without it, which is a
keyboard that never reaches a `TextBox` and nothing saying why. So
`InstallGuiServices` adds the state to whatever service the world holds and
mints one only where minting is legal: `ecs::Store::AdoptOnly` is the test,
because an authoritative index minted in a replica collides with one the
authority is handing out, and the join snapshot sweeps what it does not mention
anyway. That is what makes it safe to call once a tick on a world filling from a
snapshot, which is how the replica gets one at all.

## Typing is `Typing.hpp`, and the text it writes is the box's own

`Router` decides which box has the keyboard; `Type` decides what a keystroke
does to it. Neither knows about the other's gesture, which is why they are two
files: a press is routed by position and a character is not routed at all.

- **`Label::Text` is the buffer.** There is no edit state anywhere - no pending
  string, no undo stack, no "box being edited". Rule 2, and the specific bug a
  second copy would buy is a script writing `TextBox.Text` while somebody types
  into it and the two disagreeing until the next repaint.
- **The caret is characters and the string is bytes, and `src/Utf8.hpp` is the
  only place that crosses between them.** `Focus` counts to the end of the text
  and `Type` inserts in the middle; two copies of that arithmetic would be two
  answers to where the caret is, and both would be right for English.
- **`Type` clamps the caret before reading it, and that is not defensive
  programming.** `TextBox.Text` is a plain property with no setter to hook - the
  class table writes the field - so a handler replacing the text with something
  shorter is an ordinary event, and every offset derived from the old caret is
  then past the end. Clamping lives at the one reader that indexes by it.
- **Return releases a single-line box and breaks a line in a `MultiLine` one**,
  and the release owes a `FocusReleased` the router cannot produce, because no
  press happened. `GuiEvent::Entered` is how a script tells the two apart, and
  the caller builds that event from `TypeResult::Released`.

**A `TextBox` takes input, and that is a second class in `TakesInput` rather
than a field.** It is not a `GuiButton` and its `Active` is false by default, so
the pick walked past it and a click reached whatever was behind - which is what
made focus unreachable and, before that, made a text field ignore the mouse.
`Entry` is the test, for the reason `Element` is the `GuiObject` test below.

## `ElementsAt` asks `Pick`'s question and reads the compile's answer

`PlayerGui:GetGuiObjectsAtPosition` wants *everything* under a point, in paint
order, within one container - three ways `Pick` cannot answer. It is still not a
second traversal deciding what is on top, and the reason it does not have to be
is `Resolved::Order`: the compile writes each element's paint position back into
its own row, so **sorting by that descending is front to back**, and the section
above stays true.

Three things about it are decisions rather than details:

- **The `Active` test is dropped and every other filter is kept.** A decorative
  `Frame` is transparent to a *click* and is still an object under the pointer,
  which is exactly the difference between the two questions. `Rendered`, the
  clip and the rotation all still apply, because the answer is "what is on
  screen here" rather than "what has a rectangle here".
- **It is scoped to a root and the root is not optional.** `Layout` resolves
  every collector in the world - the `StarterGui` template and every player's
  copy - so an unscoped answer would hand one player somebody else's rectangles.
- **`Resolved::Depth` breaks a tie and the entity id breaks that.** A world
  nothing has compiled has every `Order` at zero, and the deeper element is
  still the one in front; the id is last so the answer never depends on the
  order the walk happened to visit siblings in.

`Element` is what tells a `GuiObject` from a `LayerCollector` here, and no class
lookup is needed for it - a collector has no such component, which is the same
fact the class-tree section states from the other side.

## The hover is fed back and is one frame late, deliberately

`CompileRequest::Hovered` is an *input* to the compile, and the compile is what
produces the list the hit test reads. Feeding this frame's hover into this
frame's compile would make it depend on its own output.

So a button appearing under a stationary pointer lights up on the frame after
it appears. One frame, and the alternative is a cycle.

**The lit colour never goes back into `Background::Color`.** A hover written
into the component would make `BackgroundColor3` read differently depending on
where the mouse is, which is a script bug nobody could see from the script.

## `ResetPlayerGui` is one step of a pipeline this module cannot see

`scene` copies four `Starter*` services into a player - two on the join, two on
every spawn - and this is the fifth, on every spawn. It is here rather than there
because `scene` may not link `gui`, and it stays a *function* rather than a
system for the same reason: whoever spawns calls both halves.
`scene::UpdateRespawns` hands back who it spawned so a host can loop over them,
and `mono.server`'s `player.respawn` is what does.

**It is not the same shape as the other four and must not be made so.**
`scene::CloneChildrenInto` is a plain copy; this one carries the `ResetOnSpawn`
survival rule, and a survivor of a name is what stops a player being handed two
of a collector one of which nothing updates. `Services.hpp` states the three
steps in order.

**Three spawns, not two, is what the suite asserts**, because a reset that
re-cloned every *other* time passes a two-spawn check - and the second player in
`engine.gui.services` is what catches a reset reaching the template rather than
the copy.

## An interface replicates, and four components deliberately do not

A server authors a `ScreenGui` and a client is shown it: that is what `shared`
was for, and from v0.15 it is what actually happens.
`replication::DefaultReplicatedComponents` takes the whole `gui.` prefix less
four names, and the four are the module's own answer to what a *viewer* decides
rather than what an author does.

- **`ScrollState`** joined the list at v0.18 and is `Resolved`'s case for one
  class: the pixel canvas, the visible window and the thumb rectangles are all
  derived from an `AbsoluteSize` belonging to the display doing the looking.
  What an author wrote is `Scrolling`, and that crosses.

- **`Resolved`** is where the layout put a rectangle on *this* display. Every
  client recomputes it from the tree it was sent, so the authority's answer is
  right for the authority's window and wrong for everybody else's. It is
  `scene::Rendered`'s exclusion, one dimension down.
- **`SpatialCanvas`** is the same fact fitted to a surface by whoever holds a
  camera.
- **`GuiServiceState`** holds `FocusedTextBox`, and there is one row of it per
  world. Two people typing into two boxes would be two clients writing one row -
  so this is the one that would be *wrong* rather than merely wasteful, and it is
  why `client::BuildReplicatedWorld` still calls `InstallGuiServices` every tick
  to complete a service that arrives with no state on it.

The hover and the press need no entry: `Router` holds them privately and they
are not components, which is the same fact this file states one section up from
the other side.

**`Label` and `Entry` are observed rather than signed, and that is forced.** A
signature hashes the object representation and both hold a `std::string`, whose
object representation is a pointer - so two boxes with the same words hash
differently and text edited inside its own capacity hashes the same.
`replication::Authority::Resign` declines a non-trivial component outright and
says to observe it instead.

**A `TextBox`'s text is suppressed on the wire, and that is a decision about a
person rather than about bandwidth.** `Type` writes `Label::Text` in the replica
as somebody types, so the two ends are meant to disagree - and both the delta
path and the anti-entropy audit would otherwise put the authority's copy back
over a half-typed word. `Entry` is the tag that says so, because a `TextBox` is
the only class carrying one. What it costs is that a script writing
`TextBox.Text` after a client has joined does not reach that client;
`replication/AGENTS.md` carries the whole argument.

**The caret never crossed and that is `WriteEntries`' doing.** It writes
`CursorPosition` and `SelectionStart` as `-1` rather than as themselves, so a
save file cannot restore somebody mid-edit and a delta cannot move a local
caret. Keep it that way: it is also what lets both ends of an audit hash one
value for a box only one of them is typing into.

## Text and image names intern, and that has a stated cost

`Label::Text`, `Picture::Image` and `Entry::PlaceholderText` are `core::Name`,
so text that changes every frame interns a new string every frame and takes the
process-wide registry's mutex doing it. `D00020` carries the fix and it is a
change to `ecs`, not to this module.

Until then: an example that writes text per frame is teaching the wrong habit.
`examples/Interface.luau` writes at ten hertz and says why.
