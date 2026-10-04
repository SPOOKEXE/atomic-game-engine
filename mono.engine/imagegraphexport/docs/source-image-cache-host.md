# Shared source image-cache host workflow

PrepareGraphSourceImages receives already resolved immutable controls. It
opens only explicit live file/resource grants, collects unpadded raw sprites
and optionally encodes that exact ledger. An older saved cache never replaces
the live ledger used for Cache or Match Length. Revoked live grants refuse the
refresh. Remove Cache requires no live image or file access.

Rendering an enabled saved cache uses its exact data-bound native annotation,
or a host observation naming the platform byte layout and BLAKE3 digest.
Original image file grants are unnecessary because the cache owns its bytes.
Missing, stale, duplicate or conflicting layout observations refuse. The
existing padding/precision/sequence rendering controls are applied after cache
admission. Unsupported nested or empty drawable ledgers remain explicit.

Assetc exposes repeatable --graph-image-cache-layout NODE:HASH=LAYOUT only
with --export-graph. Layout is one of the four named bake profiles. The hash
is computed from exact saved cache text. Studio provides Cache live images,
Remove cache, Match live image count and explicit foreign-layout observation
controls. Node/document/input/grant generations and prepared control snapshots
bind these actions to the frozen intent; no upstream node is executed again
just to construct its receipt. Atomic history admission precedes document and
replay publication. Undo restores the original source authoring baseline.

The host bounds raw/candidate/prior frame capacities, clone overlaps, codec
input copies and encoded text. Archive/vendor limits and existing history
quotas remain distinct from logical payload accounting. Profiling scopes and
byte/frame counters describe actual boundaries rather than inferred heap use.

Fixtures exercise real CLI subprocesses with literal argv, actual native cache
rendering without original grants, independent literal PNG pixels, stale
layout refusal preserving previous files, checked PXC save/reimport and Studio
history undo/redo. Strict checks are separate from joined runtime acceptance.
The fixture set does not claim licensed application or device parity.
