# Game UI system plan

## Status

This document defines future work for the authored game interface. It extends
the existing `mono.engine/gui` retained tree, its script surface, and the
client presentation adapters. It does not describe a second widget toolkit and
does not replace Dear ImGui in Studio before the game tree can support every
editor control Studio needs.

The terms in this document are deliberate:

- `gui` is the shared, headless game interface;
- `ui` is the client-only Dear ImGui adapter used by Studio and other tools;
- an instance is authored, saved, replicated, and visible to scripts;
- a virtual class contributes inherited class identity and components without
  creating an extra entity or Explorer row;
- derived presentation state is local output and is never authored state.

## Product goal

An author can build a complete game interface as a retained instance tree,
inspect it in Studio, drive it from either scripting language, and obtain the
same layout and interaction decisions on every supported host. The shared tree
must remain usable in a headless test or server process. A client adapter turns
its compiled commands into pixels without becoming a second source of layout,
focus, or hit-test truth.

The finished system must provide:

- complete screen and world-space interface trees;
- predictable measure, layout, clipping, ordering, and hit testing;
- responsive constraints, safe areas, display scaling, and accessibility
  scaling;
- shaped multilingual text with bidirectional layout and input methods;
- named styles, themes, state variants, and bounded visual animation;
- explicit data binding that never hides script execution in property reads;
- keyboard, pointer, touch, gamepad, and assistive navigation;
- lists and grids whose cost is bounded by visible content;
- `SurfaceGui`, `BillboardGui`, and `ViewportFrame` as first-class collectors;
- stable save and replication rules with viewer-local derived state;
- a Studio visual editor that edits the same instance tree scripts see;
- content and design import seams without embedding a browser engine;
- cascaded caches and damage tracking proportional to what changed;
- hostile input limits for trees, text, images, bindings, and nested views.

## Current foundation

### Shared retained tree

`mono.engine/gui` is an L7 shared module. It currently depends only on `core`
and `ecs`, and it already owns the correct foundation:

- the Roblox-shaped `GuiBase`, `GuiBase2d`, `GuiObject`, `LayerCollector`,
  `UIComponent`, `UILayout`, and `UIConstraint` class hierarchy;
- concrete frames, labels, buttons, text boxes, images, scrolling frames,
  canvas groups, and viewport frames;
- list, grid, table, page, flex, aspect, size, text-size, padding, corner,
  stroke, scale, gradient, and drag modifiers;
- screen, surface, and billboard collectors;
- a top-down layout pass with separate pure measurement and placement work;
- `Resolved`, `ScrollState`, `SpatialCanvas`, and `Rendered` derived rows;
- a retained `Compiled` draw list guarded by a source signature;
- hit testing and pointer routing over the compiled result;
- world-owned selection and text focus through `GuiServiceState`;
- UTF-8-aware text editing and rich-text spans;
- saved class properties and generated Luau and JavaScript bindings.

This is the system to extend. New work must not construct a parallel scene
tree in `client`, `render`, Studio, or either script runtime.

### Existing presentation adapters

`mono.engine/ui` currently paints `gui::DrawList` commands into Dear ImGui for
Studio. It resolves fonts and images, applies clip rectangles, draws gradients,
and respects pixelated sampling. This dependency points from `ui` to `gui`.
`gui` must never include `ui` or an ImGui type.

The renderer already resolves `SurfaceGui` and `BillboardGui` collectors into
spatial canvases. It also renders each visible `ViewportFrame` as a miniature
scene into a bounded target before the outer interface consumes that image.
The current implementation caps a viewport edge at 2048 pixels and caps tree
collection depth. These are useful safety rules, not temporary restrictions to
remove without replacement.

The client chooses the interface world, compiles only the correct `PlayerGui`,
routes input into that same world, and delivers resulting events to that
world's runtime. Studio compiles `StarterGui` while editing and `PlayerGui`
while running. This viewer policy remains part of the compile key.

### Existing limitations to remove

The current system has deliberate provisional edges:

- headless text measurement uses an average glyph advance;
- `AutomaticSize` and several advanced layout interactions are incomplete;
- text shaping, bidirectional layout, fallback fonts, and input-method
  composition are not complete;
- there is no localization catalogue or accessibility tree;
- large scrolling lists retain and lay out every child;
- style values are mostly repeated directly on instances;
- `PluginGui` has no host canvas, so Studio remains on Dear ImGui;
- `ViewportFrame` rendering is functional but rebuild policy and nested-view
  budgets need a complete contract;
- Studio has no production visual authoring surface for the game tree.

These limitations should be removed through the current ownership split rather
than by introducing HTML layout beside it.

## Non-negotiable rules

1. `gui` remains shared, headless, deterministic for explicit inputs, and free
   of renderer, window, input-device, Studio, ImGui, and script-VM types.
2. The ECS owns authored properties. Studio panels, bindings, renderers, and
   scripts do not keep another mutable copy.
3. One pass owns each derived answer. Layout owns rectangles, the text shaper
   owns glyph placement, compile owns paint order, and the router reads those
   answers rather than recomputing them.
4. A concrete instance has one entity. Inherited virtual classes and visual
   pseudo-states never mint hidden entities.
5. Abstract and non-creatable classes still participate in `IsA`. Hiding a
   class from insertion cannot erase its class identity.
6. Script events are emitted after routing. The L7 router never calls a script
   VM or fires a script signal.
7. Viewer-local facts do not replicate. This includes resolved rectangles,
   focus, hover, press, selection highlight, scroll-bar geometry, shaped glyph
   runs, accessibility focus, and raster targets.
8. Authored names and content references cross saves and wires. Runtime class
   ids, glyph ids, entity indices without their normal entity framing, texture
   handles, and cache addresses do not.
9. Every untrusted count, depth, string, image, list range, binding path, and
   nested viewport is bounded before allocation or traversal.
10. A cache baseline advances only after its output was completed. A failed
    shape, upload, target acquire, or draw retries the same damage.
11. UI presentation cannot steer authoritative gameplay. Gameplay reads
    explicit input or script events, not whether an animation frame rendered.
12. A simple interface remains simple. Authors do not need style sheets, data
    binding, virtual lists, or animation graphs to draw a button.

## Architecture and ownership

No replacement game-UI module is required. `mono.engine/gui` remains the
canonical owner. Larger concerns should split into focused files inside it,
with a new lower shared text module considered only when real font shaping
proves too large or reusable to remain a call-scoped service.

| Owner | Responsibility |
|---|---|
| `scene` | Service roots, players, world objects, camera data, and ordinary instance serialization |
| `gui` | Class tree, authored GUI components, virtual class metadata, measure, layout, style resolution, binding evaluation, draw commands, accessibility semantics, and input routing |
| `assets` and `delivery` | Font, image, theme, localization, UI document, and import payload bytes |
| client integration | Resolve delivered content, display metrics, locale, safe areas, input devices, and viewer identity into compile requests |
| `render` | Spatial projection, interface geometry, textures, clipping, masks, viewport targets, GPU residency, and final composition |
| `ui` | Dear ImGui host painting while Studio still uses it, sharing only declared font and image adapters |
| script adapters | Language parity over class properties, methods, signals, bindings, and services |
| Studio | Hierarchy editing, canvas tools, responsive previews, style tools, localization previews, import, undo, and diagnostics |

The architecture checks must be updated if a new text module is added. A
shared text module may own Unicode segmentation, shaping inputs, canonical
metrics, line breaking, and shaped-run values. It may not own a GPU atlas or
window APIs. Rasterization and atlas upload stay client-side.

## Instance model and virtual classes

### Concrete instances

Concrete authored controls remain ordinary ECS entities. They have stable
class names, names, parents, attributes, tags, properties, and signals. They
appear in Explorer, save in `.agame`, replicate when their container permits,
and are returned by script tree queries.

The initial concrete set remains:

- `Frame`, `CanvasGroup`, and `ScrollingFrame`;
- `TextLabel`, `TextButton`, and `TextBox`;
- `ImageLabel` and `ImageButton`;
- `ViewportFrame`;
- `ScreenGui`, `SurfaceGui`, and `BillboardGui`;
- layout, constraint, decoration, and drag components that authors need to
  parent and configure independently.

New controls should usually be compositions of these primitives. Add a new
class only when it owns behavior, state, or compatibility that cannot be
expressed as a reusable authored subtree.

### Virtual class identity

Abstract classes such as `GuiBase`, `GuiBase2d`, `GuiButton`, `GuiLabel`,
`LayerCollector`, `UIBase`, `UIComponent`, `UILayout`, and `UIConstraint` are
virtual class nodes. They contribute inherited properties and answer `IsA`,
but an author cannot insert them and the store contains no separate base-class
entity.

The class registry must expose, for every class:

- stable name and parent name;
- concrete, abstract, service, or internal kind;
- creatable and Studio-visible flags;
- inherited components, properties, methods, and signals;
- valid parent and child categories where the rule is structural;
- serialization and replication policy.

`Instance.new`, Studio insertion, deserialization, and plugin insertion all
consult this one registry. `IsA` walks its declared hierarchy for both concrete
and virtual classes. A class hidden from Studio is not removed from bindings or
from hierarchy checks.

### Derived virtual facets

`Resolved`, `ScrollState`, `SpatialCanvas`, `Rendered`, shaped text, resolved
style, accessibility nodes, binding dependency sets, and animation samples are
virtual facets. They are components or bounded cache records on the concrete
entity. They are not Instances, have no parent, do not appear in Explorer, and
cannot receive attributes or script children.

Computed properties may project a facet, such as `AbsoluteSize`, but scripts
cannot replace the facet. This keeps one entity per authored control and avoids
a hidden hierarchy whose lifetime could disagree with the visible tree.

## Hierarchy and mutation

The instance parent tree is the only authored hierarchy. Layout modifiers are
children of the `GuiObject` they affect. Collectors establish canvas roots and
do not inherit `GuiObject` geometry.

Hierarchy rules are validated at mutation and again on load:

- a collector accepts GUI descendants but cannot be nested where its canvas
  contract is undefined;
- a layout or unique modifier applies only to its parent and reports duplicate
  conflicts deterministically;
- a `ViewportFrame` owns its miniature scene descendants and camera reference;
- a `SurfaceGui` or `BillboardGui` may use a named adornee or its supported
  ancestor relationship;
- parent cycles, excessive depth, and cross-world references are refused;
- reparenting invalidates the smallest ancestor layout and compile roots that
  can change, then a safety sweep clears stale derived facets.

Batch mutation is a first-class API. A loader, Studio paste, or script creating
many siblings opens one scoped edit, writes all rows, and publishes one change
revision. Observers never see half a template or run layout once per child.

## Measure and layout

### One canonical pipeline

Layout remains deterministic and independent of a graphics device. Its inputs
are the store, collector roots, display metrics, a text-measure catalogue,
viewer policy, locale direction, and explicit time where animation affects
layout.

The pipeline is:

1. validate roots and collect the bounded child arena;
2. resolve localization, style, binding outputs, and effective visibility;
3. measure intrinsic content without side effects;
4. solve automatic sizes and constraints in declared order;
5. place parent before child;
6. resolve clips, scroll windows, paint order, and navigation geometry;
7. publish derived facets as one accepted revision;
8. compile flat draw and accessibility lists from those facets.

Measure may run more than once for a node, so it writes nothing. Placement is
the sole writer of absolute geometry. Rotation remains a presentation
transform and does not feed a rotated bounding box back into layout.

### Units, scaling, and aspect

`UDim` and `UDim2` remain the ordinary mixed scale and offset units. The system
adds explicit policies instead of inferring intent from authored numbers:

- logical-pixel scale from platform DPI;
- user interface scale from accessibility settings;
- collector reference resolution for aspect-preserving design canvases;
- fit, fill, stretch, integer-scale, and none modes;
- minimum and maximum physical hit-target size;
- aspect-ratio constraints with declared dominant axis;
- size constraints, text-size constraints, and per-axis automatic size;
- safe-area and keyboard occlusion insets;
- optional pixel snapping at the final placement boundary.

Scaling is applied once through the collector transform. Descendants do not
each multiply a global scale, which would compound it. Scripts read absolute
values in the collector's logical pixels and may separately query physical
display metrics when needed.

### Constraint order

Constraint order is stable and documented:

1. resolve authored size from the parent content rectangle;
2. apply intrinsic or automatic content size on requested axes;
3. apply aspect policy;
4. clamp minimum and maximum size;
5. apply parent layout allocation and flex growth or shrink;
6. resolve anchor and position;
7. apply visual scale and rotation without feeding them back into siblings.

Contradictory constraints choose the result by this order and emit one bounded
diagnostic carrying the affected instance. They do not iterate until a float
happens to settle. Cyclic content sizing is refused or broken at the first
declared fixed boundary.

### Layout containers

List, grid, table, page, and flex layouts share the same measure and place
protocol but keep their own algorithms. Each algorithm has stable tie breaks
for equal layout orders, uses entity identity only as the final within-process
tie break, and preserves declaration order in saved output.

Automatic canvas size is computed by the layout pass and published in
`ScrollState`. Drawing and hit testing consume that same rectangle. Elastic
scrolling is local viewer state; authored `CanvasPosition` changes remain
explicit property writes.

## Styling and themes

Direct properties remain the simple path. A button with only
`BackgroundColor3` and `TextColor3` needs no theme object.

Reusable styling adds three authored concepts:

- `UITheme`, a named content asset or instance containing typed tokens;
- `StyleClass`, a bounded ordered set of stable names on a GUI object;
- `UIStyle`, a child instance with typed declarations and optional state
  variants.

The cascade is intentionally small:

1. engine defaults;
2. collector theme tokens;
3. matching style classes in declared order;
4. local `UIStyle` declarations;
5. direct instance properties;
6. local interaction and accessibility variants.

There are no arbitrary selectors, descendant queries, specificity arithmetic,
or script expressions. A style can match a stable class name and declared
pseudo-state such as hover, pressed, focused, selected, disabled, or invalid.
Direct properties always win, so inspecting the final value is explainable.

Resolved style is a derived facet keyed by theme revision, style names, direct
property revisions, interaction state, accessibility settings, and locale
direction. Theme tokens are typed. A colour token cannot silently become a
font or number.

Studio provides a token browser, state preview, source trace for each resolved
value, and extraction of repeated direct values into a theme. Runtime scripts
may switch a theme or class name, but cannot inject a new parser language into
the frame loop.

## Text, fonts, and shaping

### One text answer

The average-advance metric is replaced by a canonical shaping path. Layout,
painting, hit testing, caret movement, selection, truncation, and accessibility
all consume one `ShapedText` result. The painter never runs its own line breaker
or measures the string again.

`ShapedText` contains owned or cache-stable values:

- font face and fallback run names;
- glyph indices local to the resolved face;
- advances, offsets, line baselines, and bounds in logical pixels;
- Unicode scalar, grapheme, word, and line-break boundaries;
- source byte ranges and visual cluster order;
- direction and script per run;
- ellipsis, wrapping, and rich-style spans;
- caret stops and selection rectangles.

The saved and replicated data remains text, font role or asset name, locale
key, and authored properties. Shaped glyph ids and atlas coordinates are local
derived data.

### Font assets and fallback

Fonts are addressed by stable content names or semantic roles. A font package
declares faces, weights, styles, Unicode coverage, metrics, licensing metadata,
and fallback order. Content delivery verifies bytes before the font parser sees
them.

A shared shaping boundary consumes validated font tables and produces metrics
without a GPU. Client presentation rasterizes needed glyphs into bounded atlas
pages. Atlas eviction cannot change layout because layout stores advances and
offsets, not atlas rectangles.

Fallback is deterministic for an ordered font catalogue. Missing glyphs use a
declared replacement face and emit a rate-limited diagnostic. Platform fonts
are not silently substituted because two machines would then produce different
line breaks.

### Rich text and editing

Rich text remains parsed into plain text and typed spans. The supported markup
is bounded, non-recursive beyond a declared depth, and has no image fetch,
script, style selector, or external link side effect. Malformed input displays
literally.

Text editing operates on grapheme boundaries rather than bytes. It supports:

- caret and range selection;
- word and line movement;
- clipboard adapter calls with explicit host permission;
- undo and redo in a bounded local edit history;
- input-method pre-edit composition and candidate positioning;
- password display without exposing clear text through accessibility output;
- single-line and multi-line submit rules;
- maximum grapheme and byte counts before mutation.

Input-method composition is local viewer state and never replaces the authored
text until committed. Script writes during editing rebase or cancel the local
selection by a documented revision rule rather than merging two hidden buffers.

## Localization and bidirectional text

Localized strings use stable keys, a source string, typed arguments, and a
named catalogue. The catalogue is a versioned asset. Authored UI replicates the
key and arguments. Each viewer resolves its own locale locally.

Resolution order is:

1. exact locale;
2. declared locale parent;
3. project source locale;
4. source string;
5. visible missing-key marker in Studio, with a rate-limited runtime report.

Formatting supports plural, select, numbers, dates, and typed placeholders
through a bounded message format. It cannot call script, read arbitrary
properties, access files, or perform network requests.

Unicode bidirectional resolution occurs before shaping and after localization.
The collector has an inherited `TextDirection` policy of automatic, left to
right, or right to left. Direction-aware start and end alignment are distinct
from physical left and right. Mirroring of layout, icons, page motion, and
navigation is opt-in per property or semantic asset. User-provided text is
isolated so control characters cannot reorder surrounding trusted labels.

Studio can preview any project locale, expand text length synthetically, show
missing and unused keys, and inspect the fallback path. Extraction and import
preserve stable keys. Renaming visible source text does not rename keys.

## Explicit data binding

Data binding is optional and declarative. It does not turn a property getter
into a script call.

A `UIBinding` child names:

- a source chosen from an allowed root;
- a typed property or attribute path;
- a target property on its parent;
- direction, conversion, fallback, and update policy;
- an optional bounded format expression from a closed operation set.

Allowed sources include the current player view model, a named world service,
a declared instance reference, a localization argument set, or a Studio preview
model. A binding resolves its path once, records exact component revisions, and
re-evaluates only when those revisions change.

The expression set includes typed field access, constants, arithmetic,
comparison, boolean selection, string formatting, and localization lookup. It
has no loops, allocation beyond declared output limits, reflection over the
whole world, network access, time reads, random values, or function callbacks.

Two-way binding is allowed only for explicitly writable local or authoritative
fields. It carries an origin revision to prevent echo loops. A failed write
leaves the source unchanged and exposes a binding error state. Replicated
client UI cannot use a binding to bypass normal authority checks.

Scripts that need arbitrary logic subscribe to ordinary change signals and set
ordinary properties. That work remains visible as script execution in the
profiler.

## Focus, navigation, and input

### Unified focus model

The world stores one GUI focus state per local viewer context, not one global
focus shared by remote players. It distinguishes:

- pointer hover and capture;
- keyboard text focus;
- navigation focus;
- accessibility focus;
- modal scope and focus restoration target.

Only the facts required by another subsystem become ECS-derived state. Pointer
gesture internals remain in the router. Destroyed or invalid targets resolve to
null through generation checks.

### Event routing

All device adapters normalize input before it reaches `gui`. The router accepts
logical events for pointer, touch, wheel, navigation direction, activation,
cancel, text, and composition. It resolves capture, tunnelling preview where
needed, target delivery, and bubbling in a fixed order.

Compatibility signals such as `Activated`, `InputBegan`, `InputEnded`,
`Focused`, `FocusLost`, and drag signals are derived from this route. A press
and release preserve the existing rule that release goes to the press origin.
Pointer capture ends on release, cancellation, target destruction, modal
replacement, or host focus loss.

Touch gestures have explicit arbitration. A drag, scroll, pinch, long press,
and button activation cannot all claim one contact. The winning recognizer is
chosen by declared priority and movement thresholds in logical pixels.

### Keyboard and gamepad navigation

Spatial navigation uses the compiled geometry and author overrides. It filters
to visible, selectable, interactable nodes inside the current focus scope,
scores direction and distance, then applies stable tie breaks. `NextSelection*`
references remain the author's final override.

Focus scopes support menus, dialogs, carousels, and nested panels. A modal traps
navigation and restores the previous valid target when it closes. Navigation
repeat uses explicit initial delay and repeat rate from the input adapter.

The action vocabulary is semantic: move, activate, cancel, page, home, end,
tab next, and tab previous. GUI code never names an SDL key or controller
button.

## Accessibility

Every visible interactive or meaningful node produces an accessibility node
from the same compiled tree. Decorative nodes are excluded unless explicitly
labelled.

The semantic surface includes:

- role, accessible name, description, value, and state;
- heading level, live-region policy, and reading order;
- labelled-by and described-by instance references;
- range minimum, maximum, step, and current value;
- supported actions such as activate, increment, dismiss, or edit;
- bounds and visibility from canonical layout;
- language and text direction.

The default role follows the concrete class, but authors can refine semantics
within compatible categories. A `TextButton` cannot claim to be an editable
password field merely by changing a string.

Platform accessibility adapters live above `gui`. They translate the compiled
semantic tree to native APIs and send semantic actions back through the same
router. The shared module can snapshot and test the tree headlessly.

Accessibility settings include UI scale, text scale, contrast theme, reduced
motion, caption preference, focus indicator strength, and input hold times.
These are compile inputs and cache keys. They do not mutate every authored
instance when a user changes a setting.

Studio audits missing names, low contrast, undersized hit targets, focus traps,
unreachable controls, ambiguous reading order, and motion that ignores reduced
motion. An audit reports evidence and does not silently rewrite authored work.

## Animation and interaction states

Two animation paths remain distinct:

- a script tween changes authored property values and follows the normal save,
  replication, and signal rules;
- a UI presentation animation samples a derived visual override and never
  changes authoritative state.

`UIAnimation` assets or instances contain typed tracks, easing, duration,
repeat policy, and named markers. Tracks target an allowlist of visual or layout
properties. Animation time is passed explicitly in the compile request. There
is no hidden wall-clock read in `gui`.

Interaction style transitions for hover, press, focus, selection, disabled,
validation, opening, and closing use the presentation path. Reduced-motion
mode shortens or removes movement while retaining visibility and state changes.
Markers used for gameplay must be delivered by the fixed simulation or script
timeline, not by whether a frame happened to sample the presentation clip.

Layout-changing animation invalidates layout for its affected subtree. Pure
colour, opacity, transform, and shader animation invalidates only compile or
geometry output. The editor warns when a large subtree is laid out every frame.

## Clipping, masks, and effects

Rectangular clipping remains the cheap default and uses nested scissor
intersection. Rounded and arbitrary masks are explicit `UIMask` operations
with bounded nesting. The renderer may implement them through stencil or
offscreen alpha targets, but the draw list describes the semantic mask rather
than a backend handle.

Canvas-group opacity, blend, blur, shadow, and colour effects require an
offscreen layer only when their semantics require one. Compile marks layer
boundaries and bounds. The renderer allocates from a capped transient target
pool and reports fallback when a requested effect exceeds device limits.

Mask and effect bounds are clipped before target allocation. A one-pixel item
cannot request a full-display target because its shader samples outside its
declared bounds. Deep mask stacks, huge blur radii, and unbounded custom shader
passes are refused.

## Lists, grids, and virtualization

Ordinary lists continue to hold concrete child instances. Virtualization is an
explicit mode for data sets whose full child tree would be wasteful.

A virtual collection contains:

- a stable item count and revision;
- a fixed or measured item-extent policy;
- a bounded overscan count;
- a stable item key provider from a declarative data source;
- one reusable authored item template;
- a visible-range output and focus retention policy.

The runtime materializes a bounded pool of presentation rows. These rows are
virtual facets, not normal instances. They cannot be found through general
world queries, saved, replicated, or assigned persistent references. Scripts
interact with the collection by stable item key and index, not by retaining a
pooled row entity.

When each item needs independent script identity, state, or replication, the
author uses real child instances and accepts their cost. Virtualization cannot
pretend pooled rows have stable instance lifetime.

Variable-height lists maintain a bounded extent index and refine estimates as
items are measured. Scroll anchoring keeps the visible keyed item stable when
earlier estimates change. Focused virtual items remain pinned or restore focus
by key after recycling.

## World-space UI

### Shared collector semantics

`SurfaceGui` and `BillboardGui` use the same descendant layout, styling,
shaping, compile, and routing code as `ScreenGui`. The renderer supplies a
`SpatialCanvas` derived from scene bounds, camera, collector properties, and
display metrics. `gui` never queries a part or camera directly.

`SurfaceGui` defines face, fixed-size or pixels-per-stud sizing, light
influence, brightness, range, z offset, always-on-top policy, and interaction.
Its hit input is a ray projected into collector canvas coordinates by the
client adapter.

`BillboardGui` defines world offset, screen offset, size, range, always-on-top,
viewer hiding, and optional distance scaling. Its facing and occlusion policy
are renderer decisions derived from authored values. Layout remains in its
logical canvas.

World-space input carries the collector id and projected coordinates into the
same router. A screen UI cannot steal a world-space pointer by accidentally
sharing coordinates. Depth, occlusion, maximum distance, and pointer ray order
are resolved before routing.

### Safe limits

Spatial collectors have caps for logical canvas extent, pixels per stud,
visible count, nested masks, generated vertices, and interactive ray tests per
frame. Distance-culling and visibility results enter cache keys only when they
change the collector's output.

## `ViewportFrame`

A `ViewportFrame` owns a miniature scene rooted under the frame and viewed by
its named camera. It remains a GUI object whose visual content is produced by
the renderer.

The production path adds:

- per-frame content and camera signatures;
- retained targets reused while scene, camera, lighting, extent, and relevant
  render settings are unchanged;
- requested resolution scale, clamped by project and device limits;
- update modes of on-change, every frame, fixed rate, and manual invalidate;
- transparent background and declared environment behavior;
- bounded nested viewport depth, visible count, target bytes, and draw cost;
- placeholder output while content or a valid camera is unavailable;
- optional pointer projection into supported interactive miniature worlds only
  after an explicit security and ownership review.

The renderer collects descendants through the same scene draw-instance path as
ordinary views. A frame is not an image asset and its target never saves or
replicates. Two frames with identical content may share immutable resident mesh
and texture data, but not camera-specific targets or damage baselines.

A `ViewportFrame` hidden by clipping, an inactive tab, zero opacity, or a closed
collector does not render. Visibility is decided before target scheduling.

## Responsive displays and safe areas

Each compile request carries a `DisplayProfile` value:

- logical width and height;
- framebuffer scale and device pixel ratio;
- orientation;
- safe-area insets;
- transient keyboard and system-overlay insets;
- pointer precision and primary input family;
- accessibility text and interface scales;
- preferred contrast, colour scheme, and reduced-motion setting.

These values are inputs, never global reads. Studio can create named preview
profiles for phones, tablets, desktop windows, television overscan, and custom
devices. A preview profile uses the same layout call as the client.

Collectors declare how they consume safe areas: respect all, ignore selected
edges, or expose a safe content rectangle while allowing decorative bleed.
Automatic layout uses the safe content rectangle. A background can still draw
to the full display without shifting every child by hand.

## Save and replication

### Saved authored state

Save files include:

- concrete instance class, name, parent, and normal metadata;
- authored geometry, appearance, text, image, interaction, and collector
  properties;
- layout, constraint, style, binding, animation, localization, and
  accessibility instances or stable asset references;
- references by the normal stable instance-reference encoding;
- enum values through versioned, pinned names or ordinals according to the
  existing component schema;
- explicit schema versions for theme, binding, animation, localization, and UI
  document payloads.

### Local derived state

The following never save or replicate:

- absolute geometry and paint order;
- resolved styles and localized strings;
- shaped glyph runs and atlas pages;
- hover, press, pointer capture, local focus, caret, composition, and selection;
- scroll-bar geometry, overscroll, and virtual-row pools;
- accessibility adapter handles;
- compiled draw commands, resident geometry, damage, and render targets;
- viewport-frame textures and device resources.

Authored scroll position may replicate when the game deliberately controls it.
Viewer-local scrolling uses a local state channel and must not make remote
players fight over one component. The class property must state which mode it
uses rather than inferring from who wrote last.

### Replication policy

Server-authored GUI intent replicates through the current `gui.` component
allowlist. Every new component declares whether it is authored, viewer-local,
or derived. Tests enumerate the registered set and refuse a component with no
policy.

Client events crossing to authority contain the player session, target stable
entity reference, event kind, input sequence, and relevant bounded payload.
The server validates that the target belongs to the sender's permitted GUI,
was eligible for that event, and that the requested gameplay action is legal.
It never trusts a client claim that a hidden button was visible.

UI delivery remains intent, not pixels. Shaped text, image bytes, draw lists,
and viewport targets are fetched or derived locally.

## Script API

The generated class surface remains the source for both Luau and JavaScript.
New properties, methods, enums, and signals are declared once and parity tests
exercise both runtimes.

The simple surface includes normal instance construction and property access.
Advanced APIs are grouped by service:

- `GuiService` for selection, focus, hit queries, display profile, safe areas,
  locale, accessibility settings, and modal scopes;
- `LocalizationService` for locale resolution, formatting, and catalogue
  diagnostics;
- `TweenService` or the existing animation service for authored property
  tweens with explicit completion;
- collection methods for keyed virtual data sources and visible ranges;
- explicit binding creation and error inspection;
- text editing methods that operate on grapheme indices.

Signals carry immutable event values. Event objects do not retain backend
device pointers. A script callback cannot run inside layout, shaping, paint,
accessibility enumeration, or a render pass.

Expensive calls return bounded results or asynchronous tickets. A script cannot
request an unbounded screenshot, force every virtual item to materialize, or
render a viewport at arbitrary resolution.

## Studio visual editor

Studio edits the exact retained game tree. It does not maintain a JSON, DOM, or
canvas-only copy that is later converted into instances.

The authoring surface includes:

- hierarchy and class-aware insertion;
- canvas selection, move, resize, anchor, rotate, padding, and constraint
  handles;
- snapping, guides, rulers, alignment, distribution, and multi-selection;
- responsive preview profiles and side-by-side locale previews;
- direct property and component editing through the normal class registry;
- layout overlays for content bounds, constraints, flex allocation, clips,
  safe areas, focus order, and virtual ranges;
- theme token and style-state editing with resolved-value traces;
- binding editor with source type checking and dependency preview;
- animation timeline for UI presentation clips;
- accessibility and localization audits;
- live draw-command, cache, overdraw, target, and event-route inspection;
- undo and redo through Studio's existing command log;
- copy, paste, prefab, and package seams that preserve stable references.

Every drag is one transaction with a preview and one committed command. The
editor does not write a property every mouse event into undo history. Preview
facets are local Studio state and are discarded on cancel.

### Dear ImGui migration

Studio remains on Dear ImGui until the retained game tree can provide a
property grid, tree, table, docking host, menus, text editor, modal prompts,
and accessibility equal to the current tool. Migration is panel by panel only
when each replacement removes the old path.

`DockWidgetPluginGui` receives a host canvas adapter when that work begins.
The adapter gives a plugin collector an extent, input route, and presentation
target. It does not allow `gui` to include ImGui or Studio.

Running both widget systems inside one panel is prohibited except for a short,
named migration bridge with a deletion gate. Two focus systems in one panel
produce input bugs that no amount of styling can hide.

## Import and interoperability seams

The canonical interchange format is a versioned UI document containing the
same class names, properties, stable references, themes, assets, and locale
keys the engine uses. Import validates into this document before creating any
instances. Creation then occurs as one undoable transaction.

Import adapters may support:

- Figma exports through a separate tool or plugin;
- SVG paths and images through the asset pipeline;
- design-token files mapped to typed theme tokens;
- localization tables;
- selected Roblox GUI model properties;
- project templates and packaged components.

Unknown properties are reported with source location and preserved in an
optional round-trip extension block only when the source format supports it.
They are not invented as engine attributes silently.

HTML and CSS are not the runtime UI model. A future HTML adapter may import a
strict static subset into the canonical document. It must reject script,
network fetches, arbitrary CSS, browser plugins, and unsupported layout rather
than shipping a hidden browser engine. A web-view control, if ever needed for
trusted documentation or account flows, is a sandboxed client widget outside
the game layout model.

## Cascaded caches and damage

### Cache layers

The interface is split into independently signed layers:

1. authored subtree and viewer policy;
2. binding and localization output;
3. resolved style;
4. shaped text and intrinsic measurement;
5. layout and navigation geometry;
6. compiled draw and accessibility lists;
7. client geometry, glyph, image, and mask residency;
8. per-collector retained image;
9. game composition and final presentation.

A write cascades upward only. Changing a colour does not reshape text or lay
out siblings. Changing text may reshape, resize automatic ancestors, compile,
and redraw. Moving a scene part holding a `SurfaceGui` changes spatial
composition without rebuilding the GUI's logical layout.

Each cache key includes only facts its producer reads. Tests walk registered
properties and verify the appropriate signature changes, extending the current
compile-signature coverage rather than maintaining a hand-written property
list.

### Damage regions

Compile records old and new conservative visual bounds for changed commands.
Their union becomes collector damage. Nested clips constrain it. Shadows,
strokes, masks, and effects expand it by declared support bounds.

The renderer may redraw damaged tiles or the full collector. It chooses full
redraw when region count, changed area, effect dependencies, or target format
make partial work slower. The threshold is measured in release builds and is a
backend policy, not baked into authored data.

Damage is correctness-first. An uncertain bound dirties the full collector.
A false extra redraw costs time; missed damage leaves stale pixels.

### Cache lifetime

Caches are bounded per viewer, collector, locale, display profile, and viewport
slot. Hidden collectors release large targets after a grace period while
keeping cheap source signatures. Font and image residency uses content names
and shared device caches. Per-frame animation samples do not mint permanent
cache entries.

Diagnostics report hits, writes, invalidation reasons, uploaded bytes, target
bytes, draw commands, glyphs shaped, visible virtual rows, and damaged area.
Cache hits are counters, not fake timing spans.

## Hostile limits and failure behavior

Project defaults and hard engine ceilings cover:

- total GUI instances and collectors per world;
- hierarchy depth and children per node;
- text bytes, graphemes, rich spans, bidi runs, and shaped glyphs;
- localization message depth, arguments, and formatted output;
- binding count, path depth, dependency count, and operations per update;
- style classes, declarations, variants, and token indirections;
- layout iterations, grid tracks, table cells, and virtual overscan;
- draw commands, vertices, indices, clips, masks, and offscreen layers;
- image dimensions, decoded bytes, atlas pages, and shader samples;
- spatial canvases, pointer ray tests, viewport frames, nested depth, target
  dimensions, and target bytes;
- queued events, text composition bytes, and script callbacks per frame.

Limits are checked before reserve, resize, recursion, decode, or target
allocation. Parsers validate into checked descriptions before building ECS
rows or GPU resources. Untrusted font, image, localization, rich-text, and UI
document parsers receive fuzz targets.

Failure is local and visible:

- an invalid subtree is skipped and named;
- missing content draws a stable placeholder;
- malformed rich text displays literally;
- a failed binding uses its typed fallback and exposes an error;
- a layout conflict uses the declared precedence and reports once;
- a target allocation failure draws the prior valid image or placeholder;
- queue overflow follows a named drop or coalesce policy and increments a
  counter;
- no failure leaves half-published derived rows as the new baseline.

## Migration from the current system

Migration stays incremental and removes provisional paths as their replacements
ship.

1. Pin the current class tree, component registration, property surface, save
   bytes, replication allowlist, layout snapshots, and compile signatures.
2. Add explicit class-kind metadata. Mark abstract hierarchy nodes virtual and
   non-creatable without changing `IsA` or concrete instance bytes.
3. Define `DisplayProfile` and safe-area inputs while preserving current screen
   defaults.
4. Add canonical shared text measurement and shaped-run output. Switch layout,
   paint, caret, and hit consumers together, then delete average-advance and
   backend remeasurement paths.
5. Complete automatic size and constraint order. Keep the old behavior as the
   schema default where compatibility requires it, not as a second solver.
6. Add style and theme resolution with direct properties winning.
7. Add localization and bidirectional shaping, then accessibility semantics.
8. Generalize routing to semantic keyboard, gamepad, touch, modal, and
   accessibility actions.
9. Add explicit bindings and virtual collections behind class-registry entries
   and hostile limits.
10. Add presentation animation, masks, damage regions, and retained collector
    targets.
11. Finish world-space collector policies and `ViewportFrame` cache budgets.
12. Build the Studio visual editor over the canonical tree.
13. Add import adapters after the UI document validator and transaction path
    are stable.
14. Port Studio panels only when each port deletes its Dear ImGui path.

At every step, old save files load, current scripts retain their simple API,
and one complete path remains shippable.

## Delivery phases and gates

### Phase 0: contracts and baselines

- Record current compatibility snapshots and performance baselines.
- Add class-kind and replication-policy coverage.
- Define limits, diagnostics, and the display-profile value.

Gate: no current example, script binding, save fixture, or headless layout test
changes unexpectedly.

### Phase 1: responsive canonical layout

- Implement safe areas, explicit global scale, aspect modes, and complete
  automatic sizing.
- Formalize constraint precedence and conflict diagnostics.
- Add layout subtree invalidation and release benchmarks.

Gate: responsive profiles produce pinned geometry without extra layout passes
in unchanged frames.

### Phase 2: production text

- Add validated font packages, deterministic fallback, shaping, segmentation,
  bidirectional text, and canonical line breaking.
- Move painting, truncation, selection, and caret behavior onto shaped runs.
- Add input-method composition.

Gate: headless layout and rendered output agree for the multilingual corpus,
and no backend measures text independently.

### Phase 3: styles, localization, and accessibility

- Add typed themes and the bounded style cascade.
- Add catalogues, formatting, locale fallback, and direction-aware layout.
- Compile semantic trees and connect native accessibility adapters.

Gate: locale, contrast, scale, and reduced-motion changes invalidate only the
required layers and pass automated audits.

### Phase 4: input and large data

- Add semantic keyboard, gamepad, touch, modal, and accessibility routing.
- Add explicit data binding and virtual collections.
- Add focus restoration and keyed recycling.

Gate: input traces are deterministic, bindings run no hidden scripts, and list
cost is proportional to visible rows.

### Phase 5: presentation depth

- Add UI animation, bounded masks and effects, damage regions, and retained
  collector images.
- Complete spatial collector and `ViewportFrame` update policies.

Gate: steady unchanged interfaces submit no interface work, hidden nested views
render nothing, and failed writes preserve damage.

### Phase 6: Studio authoring and import

- Build canvas tools, preview profiles, theme, binding, animation,
  localization, accessibility, and diagnostics panels.
- Add the canonical UI document and validated import adapters.
- Begin one-panel-at-a-time Studio migration only after parity gates pass.

Gate: every edit is undoable, Play uses the same authored tree, and imported
documents round trip without a hidden model.

## Test strategy

### Headless layout and behavior tests

The primary suite needs no window or GPU. It covers:

- empty, single, deep, wide, and limit-sized trees;
- every layout, constraint order, automatic-size axis, and scale mode;
- safe areas, orientation, DPI, text scale, and zero-sized canvases;
- stable ordering and duplicate modifier diagnostics;
- clipping, rotation-aware hit tests, scrolling, drag, and pointer capture;
- keyboard, gamepad, touch, modal, and accessibility focus traces;
- text shaping, wrapping, truncation, caret, selection, fallback, bidi, and
  composition with a fixed font corpus;
- localization fallback, plural and select formatting, output ceilings, and
  text isolation;
- style precedence, state variants, typed token errors, and reduced motion;
- binding dependency invalidation, authority refusal, cycles, and fallbacks;
- virtual-list visible ranges, recycling, anchoring, and focused-key retention;
- world-space canvas coordinates and viewer filtering;
- cache signatures and exact invalidation layers;
- save, load, replication, late join, and unknown-version refusal.

Property-walk tests ensure every registered property affects the signatures of
all stages that read it. Component-walk tests ensure each registered GUI
component has a save and replication policy.

### Image tests

A small deterministic software or reference render path produces golden images
for visual contracts. Cases cover geometry, borders, corners, gradients,
images, nine-slice and tiling, shaped text, bidi, clips, masks, opacity groups,
spatial projection, and viewport placeholders.

Golden tests use vendored fixed fonts and assets, fixed display profiles,
explicit time, and tolerant pixel comparison with a strict changed-area cap.
They do not use platform font discovery or wall time.

GPU backend tests compare selected frames against the reference output and
check resource and command statistics. Device-only tests remain narrow. The
shared behavior stays covered headlessly.

### Fuzz and hostile tests

Fuzz targets cover UI documents, rich text, localization messages, font-table
validation, style assets, binding expressions, SVG imports, and saved component
payloads. Bound tests attempt maximum depth, count, output, nesting, and target
sizes without allocating beyond declared ceilings.

### Studio tests

Studio integration tests cover selection and handles, undo transactions,
responsive and locale previews, class-aware insertion, copy and paste, import
rollback, Play and Stop restoration, and plugin collector focus. Screenshot
tests cover only stable authored canvases, not incidental operating-system
chrome.

## Profiling and budgets

Measure both `dev` for algorithm visibility and `release` for shipped cost.
Every report names build preset, backend, display profile, interface size,
visible row count, locale, font set, viewport count, and whether animation was
active.

Required counters include:

- roots, instances, measured nodes, placed nodes, and dirty subtrees;
- shaped strings, glyphs, cache hits, fallback runs, and atlas writes;
- localization and binding evaluations with invalidation causes;
- focus candidates, hit-test commands, routed events, and gesture conflicts;
- virtual items, materialized rows, recycled rows, and extent corrections;
- compiled commands, vertices, indices, clips, masks, and offscreen layers;
- source, layout, compile, residency, collector, and composition cache hits and
  writes;
- damaged pixels and full-redraw fallbacks;
- image, glyph, geometry, mask, viewport, and retained-target bytes;
- uploaded bytes, command buffers, viewport renders, and skipped hidden views;
- rejected work and every limit reached.

Release benchmarks cover:

- an unchanged HUD;
- one colour change;
- one text change inside and outside an automatic-size chain;
- a thousand real rows and a million-item virtual data source;
- rapid scrolling with mixed item heights;
- four locales including right-to-left text;
- pointer and gamepad navigation through dense controls;
- many spatial collectors at their visibility boundary;
- several unchanged and one animated `ViewportFrame`;
- resize, DPI change, safe-area change, and accessibility-scale change.

Parallel work is considered only after profiles show a useful batch above a
measured crossover. Any shaping or layout job joins before the frame publishes
derived state. Work cannot land a tick later and change which event a fixed
input trace targets.

## Open decisions

These choices need prototypes or target-platform evidence before their phase
begins:

1. Whether canonical shaping remains inside `gui` behind a call-scoped font
   catalogue or earns a lower shared text module. Decide from a working complex
   script shaper and a second real consumer, not from anticipated reuse.
2. Whether viewer-local scroll and edit state belongs in a dedicated local ECS
   resource or in a per-view router cache. The chosen owner must support several
   local view contexts without entering save or replication.
3. Which first-party font package and shaping libraries satisfy licensing,
   deterministic metrics, complex-script coverage, mobile size, and hostile
   font parsing requirements.
4. Whether virtual collection item templates need a dedicated instance class
   or can use an ordinary non-rendered subtree. Choose the path with stable
   Studio editing and no second serialization format.
5. Which native accessibility adapters ship first. The shared semantic tree is
   required before choosing platform priority.
6. Which Studio panel is the first retained-tree migration proof. It should
   exercise tables, focus, text editing, accessibility, and docking while being
   small enough to delete its old path in the same phase.
7. Whether partial collector redraw beats full retained-target redraw on each
   renderer backend. Keep damage generation either way, then choose thresholds
   from release profiles.
8. Whether a static HTML import subset earns maintenance after the canonical UI
   document and Figma or design-token adapters are proven.

## Explicit non-goals

The first complete production system does not include:

- an embedded HTML browser, DOM, JavaScript browser runtime, or general CSS
  engine;
- arbitrary script execution from style, localization, layout, or binding;
- a second immediate-mode game UI API beside the retained tree;
- platform-font substitution that changes layout between machines;
- unbounded custom shaders, masks, blur, nested views, or offscreen targets;
- remote pixel streaming as the ordinary replication model;
- stable script identity for pooled virtual rows;
- authoritative gameplay driven by rendered frames or accessibility callbacks;
- automatic conversion of every Studio panel before retained controls reach
  parity;
- a general document editor, browser accessibility model, or native-widget
  wrapper disguised as a game UI.

## Completion definition

The plan is complete when:

- `gui` remains the one shared retained model and builds headlessly;
- virtual class identity and concrete instance behavior are explicit and
  covered by hierarchy tests;
- layout, shaping, painting, hit testing, focus, and accessibility consume one
  canonical set of derived answers;
- responsive scaling, safe areas, constraints, localization, bidirectional
  text, accessibility, styles, bindings, animation, and virtual lists work
  through both script runtimes;
- screen, surface, billboard, and viewport collectors share common descendant
  semantics and have bounded client adapters;
- every authored field has save, replication, invalidation, and Studio editing
  coverage;
- unchanged interfaces do no layout, compile, upload, target, or presentation
  work for their unchanged layers;
- hostile inputs are refused before unbounded work and parser fuzz suites pass;
- headless behavior tests and deterministic image tests cover the public
  contract;
- release profiles meet recorded budgets on desktop and representative mobile
  display profiles;
- Studio edits the canonical tree with undo and no shadow document;
- any migrated Studio panel has one implementation because its old Dear ImGui
  path was removed.
