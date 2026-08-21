#pragma once

// A partition of proxies into sparse cubic chunks, rebuilt from scratch.
//
// **This is a partition and not a second index.** `HashGrid` answers "what is
// near this box"; this answers "which disjoint group is each proxy in, and
// where are that group's members". Every proxy lands in exactly one chunk,
// chosen from the centre of its bounds, so two chunks never name the same
// proxy - which is the whole property a caller wanting to run a group per
// thread needs, and the one a grid cannot offer because a box spanning cells
// appears in each of them.
//
// **Why the centre and not the bounds.** A partition has to be a function that
// gives one answer per proxy. Binning by bounds gives a set, and a caller
// splitting work by it would hand the same proxy to two threads. The cost is
// that a proxy near a chunk face pokes out of its own chunk, which is exactly
// why a consumer has to treat anything crossing a chunk boundary as its own
// case rather than assuming a chunk is a closed box. `physics::Solve` does that
// by asking whether a contact's two bodies landed in the same chunk.
//
// **The chunk is not a smaller grid.** There is no per-chunk acceleration
// structure here, and adding one would be an algorithmic change under the rule
// `AGENTS.md` states for the uniform grid: the measured curves in `HashGrid`
// already put a uniform grid ahead of a tree at every density this repository
// has scenes for, and a tree per chunk would be that same comparison repeated
// once per chunk with fewer proxies in each. What a chunk buys is *disjointness*
// and *locality*, and both are properties of the partition rather than of a
// structure inside it.
//
// **Nothing here knows what an entity is**, for `HashGrid`'s reason, and
// `AGENTS.md` in this module is the only thing that catches an edge to `ecs`.
//
// @tier L6 · shared
// @since v0.17

#include <engine/spatial/HashGrid.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::spatial {

	// Which chunk of the lattice, in chunk units from the origin.
	//
	// Signed, and `std::floor` is what produces it, for the reason
	// `GridInternals::CellCoordinateOf` gives at length: a cast truncates toward
	// zero, so the chunk at the origin would be twice the width of every other
	// and scenes are built around the origin.
	//
	// @since v0.17
	struct ChunkCoordinate {
		// The chunk's position on each axis, in chunks rather than studs. See
		// the note above for why these are signed.
		//@{
		int32_t X = 0;
		int32_t Y = 0;
		int32_t Z = 0;
		//@}

		// Orders lexicographically on Z, then Y, then X.
		//
		// **A total order and not a hash**, so the chunk list a rebuild produces
		// is a function of the contents alone. A hashed order would depend on
		// the bucket count, which depends on the proxy count, and a consumer
		// splitting work by chunk would visit them in an order that moved
		// whenever anything was added - which is the property `AGENTS.md`
		// refuses in as many words.
		//
		// Z outermost so that the order matches the ascending walk every other
		// traversal in this module uses.
		constexpr bool operator<(const ChunkCoordinate &other) const {
			if (Z != other.Z) {
				return Z < other.Z;
			}
			if (Y != other.Y) {
				return Y < other.Y;
			}
			return X < other.X;
		}

		// Whether two coordinates name the same chunk.
		//
		// @param other The coordinate to compare with.
		// @return `true` when all three axes match.
		constexpr bool operator==(const ChunkCoordinate &other) const {
			return X == other.X && Y == other.Y && Z == other.Z;
		}
	};

	// A sparse lattice of cubic chunks, each owning a run of proxy indices.
	//
	// Build one, call `Rebuild` with the proxies, then walk `ChunkCount()` and
	// ask each chunk for its members. Only chunks that hold something exist;
	// empty space costs nothing.
	//
	// @since v0.17
	class ChunkMap {
	  public:
		// Chunk edge length, in metres.
		//
		// **Thirty-two, and it is a starting point rather than a measured
		// optimum**, unlike `HashGrid::DEFAULT_CELL_SIZE`. The two numbers are
		// answers to different questions: a cell is sized so that a query walks
		// few cells and each holds few proxies, and a chunk is sized so that
		// there are enough of them to keep the machine busy while few enough
		// contacts straddle two. `SuggestChunkSize` is what turns a worker count
		// and a scene into that second answer, and a caller that wants the
		// property should call it rather than trusting this.
		static constexpr float DEFAULT_CHUNK_SIZE = 32.0f;

		// The narrowest and widest a suggested chunk may be.
		//
		// `HashGrid::MINIMUM_CELL_SIZE`'s argument, one scale up: a world
		// holding one particle would suggest centimetre chunks and produce a
		// chunk per proxy, which is a partition with no groups in it.
		//@{
		static constexpr float MINIMUM_CHUNK_SIZE = 1.0f;
		static constexpr float MAXIMUM_CHUNK_SIZE = 8192.0f;
		//@}

		// Constructs an empty map.
		//
		// @param chunkSize Chunk edge length in metres. A value at or below zero
		//                  is refused in favour of `DEFAULT_CHUNK_SIZE`, for
		//                  `HashGrid`'s reason: the alternative is a reciprocal
		//                  that reaches every later answer as a NaN.
		explicit ChunkMap(float chunkSize = DEFAULT_CHUNK_SIZE);

		// Replaces the entire contents with the partition of `proxies`.
		//
		// Indices into `proxies` are what a chunk holds, so the caller keeps the
		// proxies and this keeps no copy of them - the difference from
		// `HashGrid`, which has to hold the boxes because its candidates are
		// re-tested against them and a partition's are not.
		//
		// Capacity is retained across calls. A world rebuilt every tick over a
		// steady set stops allocating after the first one.
		//
		// Two rebuilds of the same input produce the same chunk order and the
		// same order within each chunk.
		//
		// @param proxies Everything to partition, in any order.
		void Rebuild(std::span<const Proxy> proxies);

		// Empties the map, keeping every allocation for the next rebuild.
		void Clear();

		// Changes the chunk size, emptying the map.
		//
		// Emptying is the only correct answer for `HashGrid::SetCellSize`'s
		// reason: every membership is a function of the spacing. Returns without
		// touching anything when the size did not move, which is the common case
		// and is what makes calling it every tick free.
		//
		// @param chunkSize Chunk edge length in metres.
		void SetChunkSize(float chunkSize);

		// Chunk edge length in metres, as resolved by the constructor.
		float ChunkSize() const {
			return Spacing;
		}

		// How many chunks hold at least one proxy.
		size_t ChunkCount() const {
			return Coordinates.size();
		}

		// How many proxies the last rebuild partitioned.
		size_t ProxyCount() const {
			return Owners.size();
		}

		// Where one chunk sits in the lattice.
		//
		// @param chunk An index below `ChunkCount()`.
		ChunkCoordinate CoordinateAt(size_t chunk) const {
			return Coordinates[chunk];
		}

		// The proxy indices one chunk holds, ascending.
		//
		// **Ascending, which is the property a consumer relies on.** A caller
		// walking chunk members is usually walking a second list ordered the
		// same way, and an ascending run lets the two be merged rather than
		// searched.
		//
		// @param chunk An index below `ChunkCount()`.
		std::span<const uint32_t> MembersOf(size_t chunk) const {
			return std::span<const uint32_t>(
				Members.data() + Starts[chunk], Members.data() + Starts[chunk + 1]
			);
		}

		// Which chunk a proxy landed in, as an index below `ChunkCount()`.
		//
		// @param proxy An index into the span the last `Rebuild` was given.
		uint32_t ChunkOfProxy(size_t proxy) const {
			return Owners[proxy];
		}

	  private:
		float Spacing = DEFAULT_CHUNK_SIZE;
		float InverseSpacing = 1.0f / DEFAULT_CHUNK_SIZE;

		// One proxy's chunk and its own index, which is what the sort orders.
		//
		// Sixteen bytes, so the sort moves the whole record and never needs a
		// second array to follow. The proxy index is part of the key rather
		// than payload: without it the order inside a chunk would be whatever
		// `std::sort` did with equal keys, which is not stable and would make
		// two rebuilds of one input disagree.
		struct Placement {
			ChunkCoordinate Chunk;
			uint32_t Proxy = 0;

			constexpr bool operator<(const Placement &other) const {
				if (!(Chunk == other.Chunk)) {
					return Chunk < other.Chunk;
				}
				return Proxy < other.Proxy;
			}
		};

		std::vector<Placement> Placements;

		// The chunks that hold something, ascending by coordinate.
		std::vector<ChunkCoordinate> Coordinates;

		// One more than `Coordinates.size()`: chunk `n` owns
		// `Members[Starts[n] .. Starts[n + 1])`.
		std::vector<uint32_t> Starts;
		std::vector<uint32_t> Members;

		// Which chunk each proxy landed in, by proxy index.
		std::vector<uint32_t> Owners;

		friend struct ChunkInternals;
	};

	// A chunk size measured from the proxies and from how many groups are wanted.
	//
	// **The question this answers is not `SuggestCellSize`'s.** That one picks a
	// spacing where a query walks few cells; this one picks a spacing where the
	// partition has enough non-empty chunks to keep `groups` workers busy, and
	// the two optima are nowhere near each other. A scene of ten thousand boxes
	// in a hundred-metre tray wants four-metre cells and about ten-metre chunks,
	// and using either number for the other job costs about an order of
	// magnitude.
	//
	// The estimate is the scene's own extent divided into `groups` boxes: take
	// the bounding box of the proxy centres, and choose the edge that cuts it
	// into at least `groups` pieces if the contents were spread evenly. A real
	// scene is not spread evenly, so the answer is a floor on the chunk count
	// rather than a promise about it - which is why the caller asks for several
	// chunks per worker rather than exactly one.
	//
	// **Quantised to a power of two, which is the hysteresis** and is
	// `SuggestCellSize`'s argument unchanged: a size computed exactly would move
	// every time a body was added and every move costs a full rebuild.
	//
	// **Deterministic.** One pass in the caller's order over values already in
	// the store, no clock and no address.
	//
	// @param proxies What the map is about to hold.
	// @param groups  How many non-empty chunks the caller would like at least.
	//                Zero or one asks for no subdivision and gets
	//                `ChunkMap::MAXIMUM_CHUNK_SIZE`.
	// @return A chunk size in `[MINIMUM_CHUNK_SIZE, MAXIMUM_CHUNK_SIZE]`, or
	//         `ChunkMap::DEFAULT_CHUNK_SIZE` for an empty set.
	// @since v0.17
	float SuggestChunkSize(std::span<const Proxy> proxies, size_t groups);
}
