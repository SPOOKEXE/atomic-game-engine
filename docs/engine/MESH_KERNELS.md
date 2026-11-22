# MeshKernels

`MeshKernels` is a global table of bulk mesh operations written in C++. Each
one does a full pass over a vertex stream in one call. Use them for blend
shapes, skinning, normals, smoothing and remapping in a deformer, where a
per-vertex Luau loop is too slow.

They are available in Luau and JavaScript, in game, server, client and plugin
scripts. They are deterministic: the same inputs give the same bytes on every
platform the engine ships, so a game script can use them in a replayed world.

Source: `mono.engine/script/include/engine/script/MeshKernels.hpp` (the
kernels), `scriptluau/src/LuauMeshKernels.cpp` and
`scriptjs/src/JsMeshKernels.cpp` (the bindings).

## Streams

A stream is a Luau `buffer`, or in JavaScript an `ArrayBuffer` or a typed
array (the bytes it covers). All streams are packed and little-endian:

| Stream | Element |
| --- | --- |
| positions, normals, deltas | 3 `f32` per vertex |
| weights, masks | 1 `f32` per vertex |
| triangles | 3 `u32` per triangle |
| skin indices | 4 `u16` per vertex |
| skin weights | 4 `f32` per vertex |
| matrices | 12 `f32` each, 3x4 row-major (`m0 m1 m2 t` per row) |
| weld, remap indices, CSR offsets and neighbours | `u32` |

A stream can be longer than the call needs. The kernel does not read or write
the extra bytes.

## Results

Each kernel returns `true` when it ran. It returns `false`, and writes
nothing, when a stream is too short for the count or an index is out of
range. An argument of the wrong type (not a buffer, a negative count) is an
error.

## Kernels

| Call | Does |
| --- | --- |
| `Axpy(out, a, x, count)` | `out[i] += a * x[i]` over `count` f32 values |
| `WeightedAxpy(out, weights, x, count, stride)` | `out[v*stride+k] += weights[v] * x[v*stride+k]` over `count` vertices |
| `Transform(out, input, matrices, indices, weights, count)` | Linear blend skinning with up to 4 matrices per vertex. A zero weight skips its slot. Weights are not normalised |
| `Normals(out, positions, triangles, weld?, count)` | Area-weighted normals. `weld[v]` names the vertex whose normal `v` shares, so seam duplicates get one normal. An unused vertex gets `(0, 0, 0)` |
| `Laplacian(out, positions, adjacency, mask?, iterations, amount)` | Moves each vertex `amount * mask[v]` toward the mean of its neighbours, `iterations` times (at most 256). `adjacency` is `{offsets, neighbours}` in CSR form; the vertex count is `#offsets - 1` |
| `Gather(out, input, indices, width)` | `out[i] = input[indices[i]]`, each element `width` 4-byte words |
| `Scatter(out, input, indices, width)` | `out[indices[i]] = input[i]`. A later entry wins a shared slot |

`out` can be the same buffer as the input for `Axpy`, `WeightedAxpy`,
`Transform`, `Laplacian`, `Gather` and `Scatter`.

## Example

```luau
--!strict
-- Apply a blend shape at 40 percent, then recompute normals.
MeshKernels.Axpy(positions, 0.4, smileDelta, vertexCount * 3)
MeshKernels.Normals(normals, positions, triangles, weld, vertexCount)
mesh:SetGeometryFromBuffer(layout, vertices, indices)
```

## Cost

For 25,600 vertices, `Transform` (four matrices) takes about 0.4 ms in the
`bench` build and `Normals` about 0.35 ms. Every other kernel takes less. In the
`-O0` dev build, `Normals` is the slowest at about 1 ms. See
`engine.script.bench.meshkernels`.
