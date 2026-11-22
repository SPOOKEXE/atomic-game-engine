#pragma once

// Bulk numeric kernels over packed vertex streams, for the `MeshKernels` global.
//
// **VM-free, so both adapters bind the same functions.** A Luau `buffer` and a
// JavaScript `ArrayBuffer` both arrive here as a byte span; the wrappers in
// `scriptluau/LuauMeshKernels.cpp` and `scriptjs/JsMeshKernels.cpp` only turn
// arguments into spans and a `bool` back into a value.
//
// **Every stream is packed little-endian.** `f32` streams hold IEEE singles,
// index streams hold `u32` (skin indices are `u16`). Every engine target is
// little-endian, so a stream is read with `memcpy` and no byte swap.
//
// **Deterministic, so a game script may call them.** Each kernel runs a fixed
// loop order with no threads and no SIMD reductions, and `MeshKernels.cpp` is
// compiled with floating-point contraction off, so `a * x + y` is never fused
// into an FMA on one platform and left as two roundings on another. The same
// inputs produce the same bytes on every platform the engine ships.
//
// **A length mismatch returns false and writes nothing.** Every kernel checks
// all of its streams and indices before it writes the first byte, so a refused
// call leaves `out` exactly as it was. Streams may be longer than needed; the
// tail past what the kernel reads or writes is left alone.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace engine::script::mesh_kernels {

	using Bytes = std::span<std::byte>;
	using ConstBytes = std::span<const std::byte>;

	// The most Laplacian iterations one call runs. C++ runs outside the step
	// budget, so a script cannot spend an unbounded amount of time through one
	// call; 256 iterations over 25k vertices is still a few milliseconds.
	inline constexpr uint32_t MAX_LAPLACIAN_ITERATIONS = 256;

	// The most matrices `Transform` reads. The index stream is `u16`.
	inline constexpr uint32_t MAX_SKIN_MATRICES = 65536;

	// `out[i] += a * x[i]` for `count` f32 values. Blend shapes and region
	// targets are this. `out` and `x` may be the same stream.
	bool Axpy(Bytes out, float a, ConstBytes x, uint32_t count);

	// `out[v * stride + k] += weights[v] * x[v * stride + k]` for `count`
	// vertices of `stride` f32 values each. A region mask or a falloff is the
	// weight stream (one f32 per vertex).
	bool WeightedAxpy(Bytes out, ConstBytes weights, ConstBytes x, uint32_t count, uint32_t stride);

	// Linear blend skinning: `out[v] = sum_k weights[v][k] * (M[indices[v][k]] * input[v])`.
	//
	// - `input`, `out`: `count` positions, 3 f32 each.
	// - `matrices`: 3x4 row-major f32 matrices, 12 values each (`r0 r1 r2`,
	//   each row `m0 m1 m2 t`). The matrix count is the stream length / 48.
	// - `indices`: 4 u16 per vertex. `weights`: 4 f32 per vertex.
	//
	// A zero weight skips its slot; a nonzero weight naming a matrix past the
	// end refuses the call. Weights are not normalised. `out` may be `input`.
	bool Transform(
		Bytes out,
		ConstBytes input,
		ConstBytes matrices,
		ConstBytes indices,
		ConstBytes weights,
		uint32_t count
	);

	// Area-weighted vertex normals.
	//
	// - `positions`: `count` positions, 3 f32 each. `out`: `count` normals.
	// - `triangles`: u32 triples; the triangle count is the stream length / 12.
	// - `weld`: optional, one u32 per vertex naming the vertex whose normal it
	//   shares. Seam duplicates name one canonical vertex, so a UV seam does not
	//   show as a lighting crease. Every named vertex must be below `count`.
	//
	// Each triangle adds its unnormalised face cross product (twice its area,
	// so larger faces weigh more) to its three corners, summed in double, then
	// each sum is normalised. A vertex no triangle touches gets `(0, 0, 0)`.
	bool Normals(
		Bytes out, ConstBytes positions, ConstBytes triangles, std::optional<ConstBytes> weld, uint32_t count
	);

	// Masked Laplacian smoothing (Relax, and Sag cleanup).
	//
	// - `positions`, `out`: positions, 3 f32 each. The vertex count is
	//   `offsets` length / 4 - 1.
	// - `offsets`, `neighbours`: CSR adjacency in u32. Vertex `v`'s neighbours
	//   are `neighbours[offsets[v] .. offsets[v + 1]]`. Offsets must not
	//   decrease and every neighbour must be a vertex.
	// - `mask`: optional, one f32 per vertex scaling `amount` there. Absent
	//   means 1 everywhere; 0 pins a vertex.
	//
	// Each iteration moves every vertex `amount * mask` of the way to the mean
	// of its neighbours, reading only the previous iteration (Jacobi), so the
	// result does not depend on vertex order. `out` may be `positions`.
	bool Laplacian(
		Bytes out,
		ConstBytes positions,
		ConstBytes offsets,
		ConstBytes neighbours,
		std::optional<ConstBytes> mask,
		uint32_t iterations,
		float amount
	);

	// `out[i] = input[indices[i]]`, each element `width` 4-byte words wide.
	// The element count is `indices` length / 4. Canonical vertices to display
	// vertices (and LODs). Copies bytes, so it suits any 4-byte stream.
	bool Gather(Bytes out, ConstBytes input, ConstBytes indices, uint32_t width);

	// `out[indices[i]] = input[i]`, the inverse of `Gather`. When two entries
	// name one slot the later one wins.
	bool Scatter(Bytes out, ConstBytes input, ConstBytes indices, uint32_t width);
}
