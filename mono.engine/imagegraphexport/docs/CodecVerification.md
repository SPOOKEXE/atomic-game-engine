# CPU codec verification

The live immutable-document export API was exercised with explicitly supplied
8 by 4 RGBA images and an exact encoder executable grant. This verification
used ImageMagick 7.1.2-32 Q16-HDRI, acquired as the official Linux AppImage
without a system installation. The artifact SHA-256 was
`d456cab221b5fc1c396768a026d0a33ee8665f7119c7fea7151b960c69058b21`.
It matched the release provenance subject digest; the provenance signature was
not cryptographically verified.

Artifact and provenance:

- [Official release artifact](https://github.com/ImageMagick/ImageMagick/releases/download/7.1.2-32/ImageMagick-7.1.2-32-gcc-x86_64.AppImage)
- [Published provenance](https://github.com/ImageMagick/ImageMagick/releases/download/7.1.2-32/ImageMagick-7.1.2-32.intoto.jsonl)

| Output | Independent decode result |
| --- | --- |
| PNG, automatic PNG | Exact RGBA samples and dimensions |
| Indexed PNG | Exact opaque samples; fixture alpha 128 becomes 255, alpha 0 remains transparent |
| JPEG, JPG, quality 100 | Maximum channel error 1, dimensions preserved |
| Lossless WebP | Exact visible RGBA; fully transparent pixels can lose hidden RGB |
| ICO | Exact RGBA samples and dimensions |
| BMP | Exact source-profile black flattening of partial alpha |
| EXR | FFmpeg float decode, maximum normalized component error below 0.0000001 |
| Pixel text | All 32 coordinates and RGBA tuples matched |
| GIF, WebP, APNG animations | Three frames, 40 ms per frame, exact tested colors |
| Batched and optimized batched GIF | Three frames, 40 ms per frame, exact tested colors |
| Native GIF, quality 2 | Three frames, 40 ms per frame; maximum channel error 2 |

Pillow decoded raster and animation outputs. FFprobe and FFmpeg independently
checked EXR dimensions, planar float pixels and linear transfer metadata.
EXR numeric preservation does not establish perceptual transfer-function parity.
The indexed PNG result establishes the tested alpha samples, not a universal
alpha cutoff. Native GIF uses the documented deterministic native profile;
these observations do not establish proprietary GameMaker palette parity.
TIFF was refused because it is absent from the supported source export formats.

The reproducible probe source, decoder script, output files, exact metadata and
assertion ledger are workspace build evidence in
`.cache/build/dev/evidence/pc-live-codec-check/`. Artifact acquisition metadata
is in `.cache/build/dev/evidence/imagemagick-official-7.1.2-32/`.
These paths are local evidence and are not shipped dependencies.
