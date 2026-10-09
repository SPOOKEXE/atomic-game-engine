# imagecodec

grug put shared image decoding at L8, shared tier. only core below it.
Crypto++ stays private for PNG CRC and DEFLATE. no scene, assets, filesystem,
GPU or VM here. bake and script adapters choose storage and colour space.
User approved PNG/JPEG decoding for runtime image buffers; bake keeps wrappers
but parser lives in one place.

runtime limits: width 1920, height 1080, encoded bytes 8294400, RGBA8 bytes
8294400. canonical base64 can use at most 11059200 characters. raw bytes are
top-row-first straight alpha RGBA8, no conversion. PNG/JPEG return encoded
sRGB RGBA8. progressive JPEG, interlaced PNG and unsupported channels refuse.
EditableImage import methods return a boolean result and retain pixels on the
image. Its short local ContentId remains available as an image property.
base64 payload never becomes an interned asset name.

## function logic

- Decode: inspect encoded length, dispatch explicit format, parser checks header
  extent and RGBA8 count before image storage, commit complete result only.
- DecodeRaw: validate extent and exact byte count, copy into temporary, commit.
- DecodeBase64: check bounded canonical alphabet, placement and padding bits in
  first pass; allocate exact decoded count in second pass; commit only complete.
- EncodeBase64: bound byte count and checked encoded count, fill temporary with
  standard alphabet and padding, commit complete text only.
- PNG: validate chunks and CRC, bound header before compressed copy; inflate to
  exact bounded row count, unfilter, expand channels and full-precision
  transparency keys, require IEND, commit complete RGBA8.
- JPEG: validate frame before plane allocation, bound table and coefficient
  arithmetic, decode complete scan with exact restart markers (never scan
  ahead), require EOI, upsample and convert, commit complete RGBA8.
- bake wrappers: apply explicit legacy texture limits and adapt owned pixels
  into TextureData without resampling or conversion.

## checks

parser pixel and refusal tests move with parser. bake keeps adapter parity.
new tests cover canonical base64, transactional failures, exact extent limits,
raw byte order and hostile/truncated streams. benchmark measures base64 plus
PNG/JPEG at 1920x1080, reports to terminal through `just imagecodec-bench`.

format checks follow [PNG specification](https://www.w3.org/TR/png-3/) and
[canonical base64](https://www.rfc-editor.org/rfc/rfc4648.html). decode still
uses existing supported subset; it does not perform ICC colour management.
