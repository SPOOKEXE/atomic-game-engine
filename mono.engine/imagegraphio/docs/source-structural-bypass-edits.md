# Source structural input-bypass edits

The in-memory `WritePxcxStructureEdits` transaction retains the checked source
archive and edits serialized source records. Save version `121092` is pinned to
Pixel Composer source commit `b69eca232217360cf1502ef0223523d818606652`.

## Source evidence

The source scripts establish the endpoint representation:

- `scripts/node_value_output/node_value_output.gml`,
  `__NodeValue_Input_Bypass.setIndex`, line 44: an input bypass index is
  `1000 + inputIndex`.
- `scripts/node_value/node_value.gml`, `setIndex`, lines 341 through 354:
  assigning an input's index also assigns its existing bypass junction index.
  `getBypassJunc`, near line 1093, assigns that index on access.
- `scripts/node_data/node_data.gml`, `getOutputIndex`, lines 860 through 872:
  tags `-2`, `-3`, and `-4` select their trigger or metadata junction before the
  ordinary `1000 + inputIndex` branch.
- `scripts/node_data/node_data.gml`, `deleteDynamicInput`, lines 875 through 884:
  physical dynamic slots are `input_fix_len + groupIndex * data_length`.
- `scripts/node_value/node_value.gml`, serialization near line 3007, saves the
  source junction's node ID, current index, and tags as `from_node`,
  `from_index`, and `from_tag`.
- `scripts/node_global/node_global.gml`, serialization near line 94, serializes
  each global input through its ordinary NodeValue serializer.

## Native transaction behavior

Insertion moves existing bypass connections at or after the physical insertion
slot by the number of inserted records. Deletion disconnects consumers of the
removed slots and moves surviving later connections back by the removed count.
This preserves the identity of surviving records instead of attaching a saved
connection to whichever record occupies an old ordinal. It leaves ordinary
outputs and specially tagged junctions unchanged. Both ordinary node input
records and retained `global` or `global_node` input records are visited.

Only endpoint fields change. Disconnection removes `from_node`, `from_index`,
and `from_tag`; inactive defaults, animator records, and unknown metadata remain.
New caller-supplied records keep the caller's serialized endpoint fields.
`PxcxLinkEdit` can connect to an existing input-bypass record on a known catalogue
node and refuses a missing or nonobject source input.

Deleting a source input is a native editor transaction with an explicit
serialized disconnection policy. This is not a claim about the lifetime of
removed junction objects in the licensed application. Official application
reload and interactive deletion parity have not been run for this profile.

## Bounds and atomicity

Endpoint retargeting reserves its container and input-visit work before touching
any endpoint. All edit operations share the existing cumulative ceiling of
`bake::PxcxLimits::MaximumLinks * 16` work units. The transaction also retains its
existing archive, graph, edit-count, and inserted-payload limits. These are
operation and payload limits, not a measurement of total JSON or vendor heap
residency. The meaningful retargeting scope and admitted input visits use the
existing engine profiling interfaces.

A malformed or dangling affected endpoint, stale archive identity, or work
refusal leaves caller output bytes unchanged. Successful edits are written,
read back through the checked archive reader, and imported again before
publication. If the edited source JSON is equal to the original, the original
archive bytes are returned exactly.

The focused module fixtures enable the serialized name-input bypasses and
resolve their HLSL links through compiled native input capture. The upstream
capability uses exact controls prepared by `PrepareHostCapture` and an owned,
unused fixture surface receipt. This checks the native host capability contract
and bypass routing; it makes no claim about source shader rendering. The source
bypass stores a value supplied during producer input collection, so the fixture
keeps the ordinary producer update dependency.

The module fixtures cover multi-record insert/delete, deleted consumers,
unknown fields, tagged junctions, global records, missing endpoints, stale
identity, exact no-op bytes, and cumulative work refusal. Joined release65 validation passes all three dynamic-bypass fixtures and
the complete IO suite (208 cases, 9,404 assertions). Licensed application
reload and interactive compatibility remain open.
