# editable image buffers

`EditableImage` owns row-major RGBA8 pixels. `ContentId` gives the image a
world-local runtime name such as `editable-image://42`. Use that name in an
ordinary image slot, including `ImageLabel.Image`, `MeshPart.TextureID`,
`Texture.Texture` and `ParticleEmitter.Texture`.

## raw pixels

`Resize(width, height)` clears the image and chooses its size. `FromBuffer` then
copies exactly `width * height * 4` raw RGBA8 bytes into that image. `ToBuffer`
returns an owned copy in the same row order. These existing methods keep their
larger 16,777,216-pixel storage limit.

`ColorSpace` accepts only `"linear"` and `"srgb"`. It describes the RGB bytes;
setting it does not convert them. Alpha is straight UNORM8 in either space.
Raw buffer imports preserve the image's current size and color space. Resize
before importing so the raw byte count has an exact destination.

## bounded import and export

The whole-image import methods have explicit formats:

```lua
image:FromBase64(encoded, "rgba8")
image:FromBase64(encoded, "png")
image:FromBase64(encoded, "jpeg")
image:FromEncodedBuffer(buffer, "png")
image:FromEncodedBuffer(buffer, "jpeg")
local raw = image:ToBase64()
```

The format argument is required. `"rgba8"` means canonical padded Base64 of
raw RGBA8 bytes. It uses the current image dimensions and `ColorSpace`, just
like `FromBuffer`. `ToBase64()` exports those raw pixels; it does not encode a
PNG or JPEG. Its text uses the standard [RFC 4648 Base64
format](https://www.rfc-editor.org/rfc/rfc4648): padded, standard alphabet,
without whitespace or a data-URL prefix.

`"png"` and `"jpeg"` decode a complete still image, infer its dimensions and
set `ColorSpace` to `"srgb"`. PNG alpha and transparency are retained. The
decoder accepts non-interlaced 8- or 16-bit PNG and baseline sequential 8-bit
grayscale or YCbCr JPEG. Interlaced PNG and progressive JPEG are refused.
Decoded imports replace the whole image only after parsing and size checks
succeed, so a refused import leaves the previous pixels in place.

These new methods cap imported dimensions at 1920 by 1080, raw or encoded
bytes at 8,294,400, and Base64 text at 11,059,200 characters. `ToBase64()`
returns an empty string for an image beyond the bounded export size. The new
caps do not change `Resize`, drawing methods, `FromBuffer` or `ToBuffer`.

A full 1080p RGBA8 image spans at least 1,013 ticks at the default replica
budget of eight 1,024-byte chunks per tick, before transport overhead. Reuse
imported images and update small graph inputs instead of decoding and sending
the complete Base64 payload every frame.

## image graph sources

An editable image can feed a live graph through a named string parameter bound
to the source node's `path` property. Set that parameter on the graph instance:

```lua
graph:SetInput("texture", image.ContentId)
```

Without an instance override, the bound source path comes from the string
parameter's authored default in the signed graph. `SetInput` stores bounded
name/value overrides without checking them against the authored graph first.
Resolution before evaluation rejects unknown names and type mismatches. A
declared parameter may remain unbound; its override has no node effect until a
binding uses it.
The live graph guide describes the document and world ownership rules in
[IMAGEGRAPH_LIVE.md](IMAGEGRAPH_LIVE.md).

The shared `imagecodec` module provides the PNG/JPEG decoder to both runtime
image imports and bake. Moving this reusable decoder out of `bake` is an
approved, narrow policy exception. It keeps runtime and baked image decoding on
one implementation; it does not move general asset import into runtime code.

The staged [ImageBuffers demo](../mono.engine/examples/assets/scripts/ImageBuffers.luau)
shows raw, PNG and JPEG import paths in image labels, a mesh part, a texture and
a particle emitter.

The codec suite passed 28 cases and 3,362 assertions. Normal and ASan/UBSan
runs with leak checking passed.

A local O3 benchmark decoded flat-grey 1920 by 1080 PNG, JPEG and raw RGBA8 in
about 10.4, 12.3 and 13.4 ms per iteration, respectively. These are the minimum
times from five samples, not medians; the benchmark output stayed on stdout.
