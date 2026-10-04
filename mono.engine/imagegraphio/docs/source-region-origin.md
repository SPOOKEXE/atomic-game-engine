# Retained animation-region record identity

The pinned source revision is b69eca232217360cf1502ef0223523d818606652.
`__animation_region.gml` serializes each region as `{l,c,fs,fe}`.
`project_data.gml` maps the ordered region list to `aRegion` at line852.
Its `regionUpdate` swaps crossed endpoints and sorts by start, then end.
Names are mutable and need not be unique. They cannot identify retained records.

`AnimationRegion.SourceRegionId` is an optional native authoring origin, appended
without changing the existing aggregate field order. Import assigns canonical
`pxc:region:<ordinal>` addresses scoped to that retained archive. The ordinal is
an address of a source record, not a user region ID. Moves retain the origin;
new or explicitly cloned regions leave it empty. Nonempty origins must be
canonical, at most 64 bytes and unique across the project. Labels may still be
empty or duplicate. Native format9 stores an optional `source_region_origin`
record after the ordinary regions. Older records without this marker still read.

The source inverse uses each origin to copy the complete original JSON record
before changing its four known fields. Deleting or reordering indistinguishable
regions therefore keeps their unrelated source members with the right record.
A new region creates a fresh object. Stale or duplicate identities refuse the
whole write. No label, color or endpoint matching guesses identity.

Checked reimport assigns addresses in the new archive. Only local comparison
copies are rebased to those addresses; the caller's authoring and undo owners
remain unchanged. A successful subsequent import supplies the new authority.
An actual opaque-record permutation stays a change even when all known region
values compare equal. An unchanged authoring projection retains exact archive
bytes, including unknown metadata and original JSON spelling.

Native retained-byte accounting includes region strings and table capacities.
Validation uses one fixed 4096-entry ordinal scratch array and sorting. The
inverse performs bounded direct record lookups, admits replacement backing and
copied JSON payload before growth, and preserves prior output on refusal.
The transaction payload quota is distinct from the existing archive/JSON vendor
limits; it does not claim to measure full process heap residency.

This is engine source-edit evidence. It does not assert that an official source
application retains unknown JSON members or native origins after its own save.
