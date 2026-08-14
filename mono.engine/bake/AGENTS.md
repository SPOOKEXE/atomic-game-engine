# bake — module invariants

L9 `shared`. The importers and the node pipeline: the only code in this engine
that reads a `.glb`, a `.pmx`, an `.obj`, a `.png`, a `.jpg`, a `.bmp`, an
`.svg`, an `.rbxm` or an `.rbxmx`. `assets` is what a baked mesh or texture *is*;
this is how somebody else's file becomes one.

## Nothing a shipped game links may link this

`client`, `server` and `cdn` must not name `bake` in
`mono.tools/architecture/expected_graph.json`. The build cannot enforce it —
both tiers are `shared` — so by rule 6 it is written down here, and the graph
file is the second place it is visible.

`assets/Texture.hpp` gives the reason and it is not tidiness: a parser for a
foreign format is the largest attack surface a content pipeline has, and a
client that carried one would be paying for a Huffman tree on the frame a
texture streamed in. A malformed model should at worst break somebody's build.

`shared` rather than a tool-tier idea because two things bake — the `assetc` CLI
and the studio's import — and a second copy of a glTF reader is how a format
acquires a dialect.

## This module has no filesystem, and that is load-bearing

`Graph` takes bytes and hands back bytes. No node opens a file, and no importer
follows a path — which is why `ReadGltf` refuses an external buffer `uri`
instead of reading it.

That is what makes the whole pipeline testable in a suite that opens nothing,
and it is the same trade `audio::NullDevice` makes. It also means a studio
baking into memory and a CLI baking onto a disk run identical code. A node that
reached for `std::filesystem` would end both properties at once.

## Every count is hostile and every one is checked before it is used

This runs over a directory somebody uploaded. The rule is `assets`': a length
field is compared against the bytes actually present *before* anything is
allocated. Named cases that must not be relaxed:

- **PNG**: the inflated size is checked against what the header implies, exactly,
  and the chunk CRCs are verified. A stream that produces more or less than its
  own header implies is malformed either way.
- **glTF**: an accessor's last element, computed from its stride, is checked
  against its buffer view's length. That single check is what makes an accessor
  unable to read outside its view whatever the counts say.
- **PMX**: every read goes through `Cursor`, which cannot read past the end.
  The format's index widths come out of a header byte, so a hand-rolled
  `offset +=` per field is exactly the shape that walks off a truncated file.
- **BMP**: the height is signed and means top-down when negative. Read unsigned
  it is a four-billion-row image.
- **`.rbxm`**: every read goes through `Cursor`, PMX's shape for PMX's reason.
  Three bounds are on numbers rather than on the buffer and are the ones to keep:
  a chunk's *declared inflated size*, checked before the allocation it causes
  because LZ4 expands by up to 255 times; the instance count, checked against
  `MAXIMUM_ROBLOX_INSTANCES` before a referent map is built from it; and the
  parent table's depth, checked against `MAXIMUM_ROBLOX_DEPTH` — which is also
  the cycle check, because a chain that has not reached a root within it is
  either too deep or a loop and both refuse the file.
- **`.rbxmx`**: the same tree in XML, and the bounds are different because the
  container is. **It states no length anywhere**, so there is no count to lie
  with — an XML file cannot claim an instance it did not spend bytes on, which
  is the one respect in which it is safer than its binary twin. What it can do
  instead is nest, so the element stack is bounded by `MAXIMUM_ROBLOX_DEPTH` and
  the parts of one property value by a tighter pair of their own. Instances are
  counted as they are read rather than believed from a header.
- **SVG**: the bomb is XML rather than pixels. A `<!DOCTYPE>` or `<!ENTITY>` is
  **refused outright rather than bounded** — entity expansion is a kilobyte of
  markup that unfolds into gigabytes while it is being parsed, an external
  entity is a file read by a module that opens nothing, and a drawing needs
  neither. Everything else it states is a count and every one is checked before
  it is used: the markup's length, the element count, the nesting depth, the
  path command count, the flattened point count. The raster target is bounded
  twice because it comes from two places — the caller's is checked against
  `Texture::MAXIMUM_DIMENSION` in `Image.cpp` and the document's own declared
  size in `Svg.cpp`, which also holds the area bound the canvas is allocated
  against.

  **And one bound that is on no stated count at all**, which is the one worth
  remembering: `MAXIMUM_FILL_WORK`. Four thousand full-canvas rectangles is
  sixteen thousand points and two billion pixels of compositing, and a
  ten-thousand-point polygon crossing every scanline is the same cost from the
  other end — neither is visible to a count of elements or of points. So each
  fill is charged its bounding box plus its edge-crossings *before* it runs, and
  a document past the budget is refused without doing the work that refused it.
  The number is measured against `-O0`, and raising it raises what one hostile
  file may cost in proportion.

## What is refused by name, and why it must stay refused

A half-read file that produces a recognisable, wrong result is worse than a
refusal, because it looks like a setting rather than a bug. These are refused
rather than approximated:

- **Interlaced PNG** — Adam7 is a different unfilter over seven sub-images.
- **Progressive JPEG** — the coefficients arrive across several scans and are
  refined; read as baseline it is a blurred version of the right picture.
- **Sparse glTF accessors** — reading the base alone gives the mesh before the
  override, which is geometry that is subtly and invisibly wrong.
- **Arithmetic-coded JPEG, RLE BMP, sub-byte PNG depths** — separate decoders
  wearing a familiar container.
- **Most of `.rbxm`'s value types.** The subset read is `String`,
  `ProtectedString`, `Bool`, `Int32`, `Int64`, `Float32`, `Float64`, `UDim`,
  `UDim2`, `Vector2`, `Vector3`, `Color3`, `Color3uint8`, `Rect`, `NumberRange`,
  `CFrame` and `SharedString` — which is every type this engine's own
  `ecs::PropertyType` can carry without a table it does not have. **Everything
  else is refused by name**, and two of them are refusals of principle rather
  than of effort: an **enum** is a number naming a member of Roblox's table, and
  honouring one means either shipping Roblox's numbering or using this engine's
  declaration order, which is exactly the id-derived-from-order rule 4 forbids; a
  **reference** is a number naming a row of the file, and a referent becomes the
  shape of the tree and nothing else.

  **A refused type costs its property and not its file**, which is a property of
  the format rather than a kindness: a `PROP` chunk holds one property of one
  class and nothing after it depends on its bytes, so an unknown type is a chunk
  skipped whole. That is what makes a partial reader of this format safe rather
  than lucky, and it is why the list can grow one row at a time.

  **`ProtectedString` is the row nobody would think to add.** It is a separate
  type number carrying identical bytes to a `String`, and it is what a script's
  `Source` is written as — so a reader that knows only `String` imports every
  script in the file with no program in it and reports nothing worth reading.

  **The XML container has the same row wearing a different hat.** There a
  `ProtectedString` is written inside a `<![CDATA[ ]]>` section, which a scanner
  that treated `<!` as a declaration refuses and one that treated it as markup
  mangles. Both were found by reading real files rather than by reasoning about
  the format, which is the method to use on the next row of either list. Two
  more are XML-only and are read rather than refused for the same reason:
  `Content` and `BinaryString` are separate elements here for what the binary
  container stores as one `String`, so refusing them would make a `Decal`
  imported from XML lose a texture the same model keeps as an `.rbxm`.
- **Most of SVG.** The subset drawn is `<svg>`, `<g>`, `<rect>`, `<circle>`,
  `<ellipse>`, `<line>`, `<polyline>`, `<polygon>` and `<path>` with M, L, H, V,
  C and Z; solid `fill` and `stroke` with their opacities and `fill-rule`; and a
  `transform` of `translate` and `scale`. **Everything else is refused by name**
  — `<text>` needs fonts and shaping, `<image>` and `<use>` need a reference
  graph, gradients, filters, masks and group `opacity` need an offscreen
  compositor, `style` and `class` need CSS, `rotate` and `matrix` turn a stroke
  into an elliptical pen, arcs and quadratics are a different parameterisation,
  and a unit that is not `px` needs a font, a viewport or a DPI. `<title>`,
  `<desc>` and `<metadata>` are skipped whole rather than refused, because they
  carry no marks. Half-drawing any of the rest gives a picture that is
  recognisably the right icon and wrong, which is the failure the whole of this
  section is about.

## One XML scanner, and it is not this module's any more

`core::xml` is the tag scanner both `Svg.cpp` and `RobloxModelXml.cpp` run on.
It was `Svg.cpp`'s private copy until v0.15, this module's `src/Xml.hpp` for the
rest of that version, and `core/Xml.hpp` from `D00128` — each move happened when
another format wanted markup, for the reason the box filter moved to `assets`:
two copies of a thing that refuses a `<!DOCTYPE` are two places to keep that
refusal true, and the second to be edited is the one that gets forgotten.

**Vendoring was the alternative and it was argued rather than assumed.**
`mono.vendor/AGENTS.md` carries the whole of it; the part to remember here is
that vendoring would not have removed a hand-written parser from this
repository, because this module already had one and `game/Xml.cpp` has had a
second since v0.7. The choice was one scanner or a submodule *and* a scanner,
and consolidating deleted code rather than adding a dependency.

**`game::ParseXml` is still not called from here and the reason is still the
tier.** `game` is L10 and this is L9, so an importer naming it would put `ecs`,
`world` and the save format underneath a foreign-format parser — the same
argument `RobloxModel.hpp` makes about `game::PropertyValue`. What `D00128` did
was move the scanning *down* to L1, where every caller can reach it; `game` now
builds its document over the same scanner these two do.

**Settings are this module's and the scanner is nobody's.** `Svg.cpp` and
`RobloxModelXml.cpp` each bind an `xml::Options` — the name a refusal calls
itself, the attribute bound, and that a namespace prefix means nothing to either
format — and each flattens `xml::Failure` to the string it reports. Neither is
allowed to reach into the other's.

Three properties of that scanner are the ones a change must not lose, and each
has a test in both suites here as well as in `core/tests/Xml.cpp`:

- **A `<!DOCTYPE` or `<!ENTITY` is refused rather than bounded.** There is no
  code here that could expand an entity, so there is no option that could switch
  one on — which is a stronger statement than a library's default.
- **An entity reference that is not one of the five predefines or a numeric
  character reference is refused where it is read.** The second lock on the same
  door, so that a bomb reads as a bomb rather than as a dropped character.
- **Nothing recurses.** `NextTag` scans and the caller keeps the stack, so depth
  is a count somebody bounded rather than the C stack running out with no file
  named.

**The document-wide entity sweep is `Svg.cpp`'s and must not be copied to
`.rbxmx`.** An SVG never unescapes, so a reference has to be caught by a sweep;
an `.rbxmx` holds CDATA, and a real file in this repository's own corpus carries
the Luau pattern `"[&;]"` inside a script — a sweep refuses that file while
naming an entity nobody wrote. Both policies live in `core::xml` and neither is
the default, because which one is right is a property of the format rather than
of the scanner. Each has a case that goes red if the two are collapsed: "an
rbxmx script's ampersand is source and not a reference", and the CDATA half of
"an svg's document type declaration is refused outright".

## Roblox's model containers are the one importer here whose output is not a mesh or a picture

Every other reader hands back an `assets::MeshData` or an `assets::TextureData`.
A Roblox model is an *instance tree*, so `ReadRobloxModel` and
`ReadRobloxModelXml` hand back a tree of class names, instance names and values,
and who turns that into rows in a store is the caller's problem — `bake` is L9
and knows nothing about `ecs`.

**Two containers, one `RobloxModel`, and that is not negotiable.** The binary
and the XML are the same tree written twice, so they produce the same types and
go through the same mapping in `studio::RojoSync`. A second model type for the
second container would be the copy that drifts, and
`tests/RobloxModel.cpp` holds the case that stops it — one model written both
ways, asserted field by field to come back identical. The subsets are the same
list for the same reason; where they differ it is in what the *format* can do,
and each difference is written down in `RobloxModel.hpp`.

Two consequences worth stating, because both look like duplication until you
check the tier:

- **`bake::RobloxValue` is not `game::PropertyValue` and cannot be.** That type
  is L10 and links `ecs`, `scene` and `world`; an importer naming it would put
  half the engine underneath a foreign-format parser. So this carries the kinds
  a `.rbxm` can produce and the caller converts, **keyed on the type its own
  class table declares** rather than on what the file stored — which is the rule
  `studio::RojoSync`'s JSON path already follows, one format along.
- **Nothing that comes back is numbered.** The tree is nested rather than a flat
  list with indices, so a referent — a number an author's copy of Studio chose —
  cannot leak out as identity. It becomes the shape of the tree and dies with
  the parse.

Its own LZ4 block decompressor is where this module departs from
`mono.vendor/AGENTS.md`'s preference for a submodule, and the argument is at the
code in `RobloxModel.cpp`. Zstandard is the opposite trade and is vendored. The
XML scanner was the second such departure and is `core/Xml.hpp`'s since
`D00128`, argument included.

**A refused property costs its property in both containers, and the reason is
not the same one.** A `PROP` chunk holds one property of one class and nothing
after it depends on its bytes; an XML element carries its own end tag. The
second is the stronger property, so the XML reader goes one step further: a
value that is *malformed* rather than merely unknown is skipped there, where in
the binary container a payload that ran short has lost the cursor and ends the
file.

## The two conversions that are not optional

- **PMX is left-handed.** Z is mirrored *and the triangle winding is reversed*.
  Doing one without the other gives a model that renders inside-out, which reads
  as the renderer having lost its culling.
- **OBJ's `v` runs up the image and everything else here runs it down.** Flipped
  once, at import. A renderer that flipped instead would flip the formats that
  were already right.

## JPEG chroma upsampling is libjpeg's, deliberately

`Upsample` uses libjpeg's triangle filter weights, rounding included, on any
axis subsampled by exactly two. The first version sampled nearest with a comment
claiming that matched other decoders; measured against Pillow it was 28 levels
out on a saturated chroma edge and the filtered version is 3, which is the
IDCT's own rounding. Do not "simplify" it back.

## Crypto++ writes a CRC-32 digest in host order

PNG stores it big-endian. `CRC32::Verify` against the four bytes in the file
therefore disagrees with itself on every little-endian machine, which is every
machine this builds on. Both sides are assembled into a `uint32_t` and compared
as numbers. This cost an afternoon once.

## The box filter is `assets`', and it left here at v0.15

`ResizeImage`, `MipChainLevels` and `BuildMipChain` were this module's until
v0.15 and are now `assets::`, declared in `assets/Resample.hpp`. The move was not
tidying: `assets` is L8 and cannot see L9, so its own generated textures — the
built-in checker, and the two sheets `render` compiles in — could not be given a
chain by a filter sitting up here, and all three shimmered. `assets/AGENTS.md`
carries the argument and the flipbook stopping rule that goes with it.

Nothing was left behind as a wrapper. `Graph`'s `Resize` and `Mipmap` arms call
`assets::` directly, and a second box filter in this module is how two textures
would start disagreeing about what a half-size copy of themselves is.

What stayed is everything that reads a foreign file: this module still owns
turning a PNG, a GIF, a glTF or an SVG into an `assets::TextureData`, and
`assets` still owns what happens to one afterwards.

## An SVG is identified by its name, and its size is a node's parameter

Two rules, and both are about the same fact: an SVG carries neither a signature
nor a pixel.

**The name identifies it, because the bytes cannot.** `ImageFormatOfBytes` is
the preferred answer for every other format and deliberately does not sniff
`<svg` or `<?xml` — a prefix over text is a claim over every text file that
starts that way, which is the argument `Graph`'s import dispatch already makes
about a `.gltf` being JSON. `ImageFormatOfName` is asked only where the bytes
said nothing, so a `.svg` holding a PNG still decodes as a PNG.

**The raster size is a `Rasterize` node and not a `Resize` after an `Import`.**
An SVG states a coordinate system, so somebody has to choose the pixels;
rasterising large and box-filtering down is a different picture, with edges
belonging to the resampler rather than to the shapes. A zero target means the
size the document declares, which is the only size it can be said to have, and
`Import` handed an SVG refuses while naming the node that would have worked.
`assetc` applies `--max-texture` to a drawing by rasterising it again at the cap
rather than by adding a resize.

## The `Mipmap` node goes last, and the graph does not enforce it

Every other texture node changes the pixels the levels are filtered from, and
`assets::ResizeImage` drops the chain outright — so a `Mipmap` before a `Resize` reaches
disk with no levels and nothing saying why, and one before an `Opaque` leaves the
levels' alpha as it was. `Graph` cannot check this: a node knows its input and
not what is downstream of it. Rule 6, so it is written here, and `assetc`'s
pipeline puts the node immediately before the write.

## What is not here yet, so nobody adds half of one

- **No skinning.** Joints and weights are skipped and the rest pose is kept.
  A glTF node carrying a `skin` contributes the identity rather than its own
  transform, which is the specification's rule and is what stops a skinned model
  importing on its side.
- **No materials.** `AssetKind::Material` names a kind nothing writes.
  `Submesh::Material` is what the source file called a run and `Submesh::Texture`
  is an asset that exists; when a material format arrives, the second becomes
  what that material references.
- **No model writer, and no `.rbxl` or `.rbxlx`.** Both readers read; nothing
  here produces either format. A place file is the same container with more
  services in it and would read today, but nothing asks for one — Rojo's table
  maps a *model*.
- **No animation, no morph targets, no rigid bodies.** PMX carries all three and
  this reads none of them. The index widths are parsed anyway so that the day
  there is a consumer, the cursor already knows how to step over them.
- **OBJ polygons are fan-triangulated**, which is right for a convex face and
  wrong for a concave one. OBJ carries nothing that says which.
