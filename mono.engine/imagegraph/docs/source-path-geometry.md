# Source Path Transform and Remap Path

Pinned source: Pixel Composer `b69eca232217360cf1502ef0223523d818606652`.

| Route | Source file | SHA-256 |
| --- | --- | --- |
| `pc.path_transform` | `scripts/node_path_transform/node_path_transform.gml` | `19b1f2f5542482a8fec2a8f235866de54832f1b9a18fa245c86cdff52bffdabe` |
| `pc.path_map_area` | `scripts/node_path_map_area/node_path_map_area.gml` | `a8d3604ac1e70e977779233df61a5045c4021fb0b14dbfa484b2d70c6a647f67` |

Transform preserves the source anchored scale, rotation and translation order,
including its exact zero and half-turn rotation branches. Source bounds transform
only the two opposite child boundary corners. Sampling can therefore fall outside
those reported bounds. Child lengths and weights remain unchanged.

Remap Path implements all three source and all three target choices. Its input
boundary becomes minimum plus extent; target Area stores center plus half extent.
Sampling preserves the source divide, multiply by half extent, then multiply by
two order. Signed target extents remain signed. Distance sampling divides by the
first child line length even when sampling another line.

The source Area control does not call setUnitSimple. Direct authored Area fields
are consumed raw, including their existing Mode and Shape values. The source
getter checks the originating NodeValue display_data, not the originating node
itself. Ordinary outputs terminate provenance at __NodeValue_Output, which has
no surface callback. Area and group Area outputs setDisplay without carrying the
parent display_data, so those links remain raw and context-independent.

Source input bypasses publish from_junc.getValue before terminating provenance
at the bypass output. An Area input using setUnitSimple can normalize padding or
two-point fields and apply its producer surface reference during that getter.
Native bypass storage currently forwards the resolved input value without that
owned getter observation. Remap Path therefore diagnoses Area-unit input bypass
links before publication. Its input producer identity is available, but upstream
resolved controls and surface-context observations are not available to this
executor. Area bypasses without a unit callback remain valid and raw; ordinary
Area outputs remain valid. No surface conversion is inferred from a node id.

Getter evidence: node_value_area.gml valueProcess/getValue;
node_value_output.gml getValueRecursive; node_value.gml setDisplay/setUnitSimple;
node_data.gml input_bypass publication; node_group_input.gml Area display branch.

The source Remap processor keeps its prior wrapper when its path becomes noone.
Native evaluation captures the owned last value per processor row in DataReplay.
Initial noone yields the source constructor boundary [-1,-1,1,1] and zero samples;
selected mapping controls do not replace it. Transform creates its configured
wrapper even for initial noone and uses the source zero sample fallback.

These kernels accept bounded planar path trees. Spatial paths are diagnosed.
Zero input mapping extents would produce source nonfinite coordinates; the finite
native path profile refuses them before publishing a candidate. Recursive path
validation precedes the whole-processor-batch work limit of 2^24 reference work
units. Clone and replay payload bytes are admitted before owned allocation.

The graph suite covers rotation order, source boundary quirks, all mapping choices,
signed/raw Areas, project defaults, noone constructor/retention, persistence,
budget failure, dense processor-batch refusal, safe linked outputs/raw bypasses
and atomic refusal of unobserved Area-unit bypass getter context. All 14 cases pass
in the joined release56 core suite. Licensed application parity remains open.
