# Source point triangulation

`pc.points_triangulate` consumes the complete point list in source slot 0 and
publishes slot 0 as `[triangle][corner][x, y, 1]`. The numeric weight is retained,
including for packed Vec2 inputs. The catalogue projection exposes an Array
output; the runtime socket domain preserves the source Float declaration and
Vector display. A coordinate pair becomes one point. Empty, duplicate and
collinear inputs retain the bounded helper's source behavior.

The inspected source is commit `b69eca232217360cf1502ef0223523d818606652`,
`scripts/node_points_triangulate/node_points_triangulate.gml` and the embedded
C++ in `scripts/delaunay/delaunay.gml`. It calls triangulation without a polygon
filter. The native helper therefore has an explicit unfiltered option; existing
mesh callers retain their polygon filter and winding behavior. Point outputs
retain the helper's order and integer-coordinate hash collision behavior.

Admission includes overlapping topology/hash scratch and owned output staging.
Each insertion scans existing triangles; each bad triangle removal scans again.
The executor bounds this conservative cubic work across the whole processor
batch before running the helper. Coordinate/topology, allocation and byte
refusals publish no output. This is source-derived CPU behavior, not licensed
runtime parity. Joined tests have not yet run for this addition.
