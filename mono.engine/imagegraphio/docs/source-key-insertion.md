# PXC object key insertion

Source pin: b69eca232217360cf1502ef0223523d818606652.

Gradient replacement requires retained serialized JSON, and Matrix replacement
requires an existing matrix array or object so their unknown fields stay intact.
Fresh inserted keys have no retained descriptor. Their dedicated encoder emits
only the fields specified by the pinned value serializers, while existing key
replacement preserves its retained source fields.

Pinned source proof:

- scripts/node_keyframe/node_keyframe.gml:657-680: valueAnimator.serialize invokes
  val.serialize() on a serializable struct and uses the returned value as key
  record slot1. The newly inserted key uses the existing verified marker/ease/
  driver/colour record encoder.
- scripts/gradients_function/gradients_function.gml:26-32: gradientKey.serialize
  emits exactly {time,value}. Lines287-294: gradientObject.serialize emits a JSON
  string with exactly {type,keys}; key records come from gradientKey.serialize.
- scripts/__matrix/__matrix.gml:1-18: Matrix stores size=[columns,rows],
  isize=columns*rows and row-major raw cells. Lines155-157: Matrix.serialize emits
  exactly {size,isize,raw} as an object. It does not serialize a JSON string.

EncodeInsertedValue is private to the existing editor implementation. It emits
these fresh descriptors for new Gradient/Matrix values and delegates all other
values to the existing seed-free encoder. New values carry no opaque fields from
a neighboring key. Unchanged records, source node/input/project unknown metadata
and surviving record tails remain retained. Existing native transaction limits
admit finite cells, gradient key counts, payload size, native key count and
sequence work before descriptor construction.

The new suite builds checked PXCX archives using Node_Gradient_Out and Node_Matrix,
with signed integer key times -2,5,12, differently shaped new Gradient key lists
and rectangular2x3 matrices. It writes edits, checks canonical source fields and
unknown retention, reimports, compiles and evaluates exact inserted endpoint
values, then checks native Write/Read equality. Nonfinite/malformed values and
oversized correct-shape payloads must preserve prior output bytes.

Persisted Gradient interpolation and exact Matrix endpoints pass the joined CPU
suites. Interior source Matrix intervals remain unsupported because the pinned
Matrix object has no interpolation override. Fractional multi-key maps remain
explicitly unsupported. These are source-backed native format results; licensed
application acceptance remains open. The final release-tests build passes all
179 imagegraphio cases (6,442 assertions) and 1,552 imagegraph cases
(1,814,785 assertions). Evidence is retained under
`.cache/build/dev/evidence/pixel-composer-2026-10-02/` in the
`io-release-56-final.log` and `core-release-56-final.log` files.
