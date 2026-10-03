# Source input record identity

Dynamic HLSL source inputs have positional names, while the retained PXC record
can contain unknown headers, empty animator arrays, key tails and disabled
programs. `DynamicInput.SourceInputId` identifies the original complete record by
`pxc:input:<original physical input index>`. It is independent of the current
native port name and is scoped to its authored source node.

The optional native format9 record is:

```text
source_input_origin 0 "shader" "argument_value_2" "pxc:input:13"
```

The ordinal is canonical unsigned decimal, bounded to 64 bytes, without leading
zeros. Empty origins denote new inputs. Origins are unique within each node.
Native read validates them with one fixed ordinal scratch array and sort;
compile uses its admitted node-local ID set. Borrowed scratch and logical retained
payload are distinct from heap allocation measurements.

The source writer moves whole name/type/value triples before applying edits to
keys or defaults. It rejects missing, duplicate, foreign-node or incomplete
origins and incompatible roles. It preadmits requested move-table and
replacement-array slots, reserves their empty backing once, then charges any actual excess capacity before copying
records. Native canonical tuple backing follows the same rule. Record copies,
lookup storage and conservative fresh-value JSON expansion are also admitted
before their operation. The operation cap bounds logical projection payload and
these explicit backing capacities; it is not a process heap-residency cap or an
exact measurement of vendor JSON object/container allocations. The separately
bounded source JSON and candidate archive can coexist with retained input data.
New records use the existing source value encoder. Existing unchanged animators are
retained exactly, including empty key arrays and unknown fields.

Incoming routes compact with the input names. Outgoing source input bypasses
rewrite `from_index` using the same record move table. A transaction that deletes
that route clears its old source connection fields while retaining local defaults
and unknown fields. A remaining native reference to a deleted record refuses.
The source rule comes from pinned `node_data.gml:getOutputIndex`, which maps
indices above 1000 to the corresponding input's bypass junction.

Checked archive reimport rebases comparison origins to their new physical
positions. Adopting that saved import supports another edit/save without using
stale indices. Writer failure preserves both the caller document and prior output
bytes. New-node authoring and cloning must clear inherited record origins and
source key provenance; an uncleared clone cannot consume another node's records.
Runtime replay copies retain the authored identity.

Headless acceptance covers distinct unknown headers despite identical names and
values, middle deletion, empty animators, signed/fractional keys, disabled programs,
sampler links, surviving and removed bypass routes, native Write/Read, successive
save/adopt, fresh records and pre-encoding budget refusal. The annotations are
engine-owned retention metadata. Official application retention is unverified.
