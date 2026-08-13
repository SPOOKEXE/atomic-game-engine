# bake — module invariants

L9 `shared`. The importers and the node pipeline: the only code in this engine
that reads a `.glb`, a `.pmx`, an `.obj`, a `.png`, a `.jpg`, a `.bmp` or an
`.svg`. `assets` is what a baked mesh or texture *is*; this is how somebody
else's file becomes one.

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
- **No animation, no morph targets, no rigid bodies.** PMX carries all three and
  this reads none of them. The index widths are parsed anyway so that the day
  there is a consumer, the cursor already knows how to step over them.
- **OBJ polygons are fan-triangulated**, which is right for a convex face and
  wrong for a concave one. OBJ carries nothing that says which.
