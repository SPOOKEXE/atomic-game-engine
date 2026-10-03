# Polar Mirror path getter profile

Pinned source hashes are in [source-mirror-path-getter-sha256.json](source-mirror-path-getter-sha256.json).

The native CPU getter supports a Path2D object carried through a source-compatible
Any input to the five Polar Mirror Vec2 controls: Relative Dimension, Constant
Dimension, Position, Center and Scale. A directly declared Path output still
cannot connect to a Vec2 socket, matching the pinned source's disabled directional
cast. Existing curve and lazy path operation semantics come from PathRuntime.

The consumer's own raw animator X is the path ratio, before processor row
selection. Y does not affect the path sample. The getter samples source line zero
and returns physical XY before validators or Reference units. Imported linked
controls retain their local raw pair, keys, driver/end settings and source flags;
PXC inverse edits keep the connection and unknown source fields.

Getter animation mode can be inherited from the immediate source instance while
local animator storage remains on the consumer. GroupReplay bindings additionally
retain the original animator writer. Static getters read the first stored key;
a one-key driver is evaluated only when the effective getter invokes its animator.
A saved empty r reloads the source constructor key. An explicitly emptied live
raw tuple has undefined X and receives UnsupportedExecution without publication.

EvaluationInputValue owns the prepared physical Vec2 and its original
SourceSocketDomain. Mirror evaluates in the same graph transaction using private
five-port provenance to distinguish path coordinates from numeric Reference-unit
pairs. No production consumer rebuilds Mirror from an input snapshot, so no
public replay marker is added. Snapshot admission uses the complete value slot
and payload capacity. Context views borrow only during evaluation; replacement capacity is charged
alongside the old views before processor outputs are produced. Path runtime
scratch follows existing bounded path admission and is released after sampling.

Separated axis animators remain a named authored-storage gap. Import refuses
that Mirror route instead of discarding axes or inventing a ratio. The outside
split-axis proposal identifies independent scalar key storage, shared property
mode/end controls, GroupReplay aliases and required native/PXC/capture codecs.
This CPU profile does not claim licensed executable or GPU raster parity.
