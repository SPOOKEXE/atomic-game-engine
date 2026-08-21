// Assembling a game's assets into the units that get streamed.
//
// **Grouping runs once per publication and decides how the game loads for
// everybody who ever downloads it**, which is an unusual place for a benchmark:
// nothing here is on a request path and no player ever waits for it. What it is
// on is the *publish* path, and a publish is the loop a person iterating on
// content sits in. A pipeline that takes a minute to group fifty thousand
// assets is a pipeline somebody stops running, and the first thing that gets
// skipped is the step that decides whether the game loads progressively.
//
// **The size of the input is the whole risk.** Grouping applies three rules in
// order - affinity, then the size mix, then priority - and every one of them is
// a relation between assets rather than a property of one. Written carelessly
// that is quadratic, and quadratic is invisible at the thousand assets a test
// uses and ruinous at the fifty thousand a real game ships. So the rows climb
// by an order of magnitude twice, and the figure to read is not any one of them
// but whether the third is ten times the second or a hundred.
//
// **The shapes matter as much as the size.** Affinity outranks the size bound,
// so content that binds heavily is a different code path from content that
// binds not at all, and the oversized case - one affinity heavier than the
// maximum a group is packed to - is the branch that has to widen a group rather
// than split it. Each gets a row, because the pathological input is the one a
// real game has and a synthetic test does not.
//
// Deterministic by contract: two origins that grouped the same content
// differently would prepare and cache different bundles for it, and nothing
// anywhere would report that they had stopped sharing. That is checked in
// `tests/Grouper.cpp`; here it means a row is comparable between machines.

#include <engine/assets/ContentHash.hpp>
#include <engine/testing/Bench.hpp>

#include <cdn/Grouper.hpp>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>

TEST_SUITE_ID("cdn.bench.grouping")

using cdn::Assembly;
using cdn::GroupCandidate;
using cdn::Grouper;
using cdn::GroupPolicy;
using engine::assets::ContentHash;
using engine::testing::Consume;

namespace grouping_bench {
	// A thousand, ten thousand, fifty thousand. The last is what a shipped game
	// looks like once meshes, textures, materials and colliders are counted
	// separately, which they are.
	constexpr size_t SMALL = 1000;
	constexpr size_t MEDIUM = 10'000;
	constexpr size_t LARGE = 50'000;

	// A distinct root per candidate, derived rather than hashed.
	//
	// **Not `Hasher::Of`**: at fifty thousand candidates the setup would spend
	// longer hashing than the measured body spends grouping, and the setup runs
	// once per sample. What grouping does with a root is copy it and sort by
	// it, and both are the same cost whatever produced the bytes.
	ContentHash RootOf(uint64_t index) {
		ContentHash root;
		for (size_t byte = 0; byte < root.Digest.size(); byte++) {
			root.Digest[byte] = static_cast<uint8_t>((index >> (byte % 8 * 8)) ^ (byte * 31));
		}
		return root;
	}

	// The size distribution a real publication has: mostly small files with a
	// long tail of large ones, rather than a uniform spread. A uniform input
	// would make the size-mix rule look easy, and easy is not what it is.
	uint64_t BytesOf(uint64_t index) {
		const uint64_t bucket = index % 100;
		if (bucket == 0) {
			return 8ull * 1024 * 1024;
		}
		if (bucket < 10) {
			return 512ull * 1024;
		}
		return 4ull * 1024 + (index % 977) * 64;
	}

	// Candidates built once per shape and kept, so a measured body groups and
	// does nothing else.
	//
	// @param count      How many assets.
	// @param affinities How many distinct affinity groups to spread them over,
	//                   or zero for "everything belongs with nothing".
	// @param bands      How many priority bands, at least one.
	const std::vector<GroupCandidate> &Candidates(size_t count, uint32_t affinities, uint32_t bands) {
		struct Shape {
			size_t Count;
			uint32_t Affinities;
			uint32_t Bands;
			std::vector<GroupCandidate> Built;
		};
		// A deque rather than a vector: a reference handed out by an earlier call
		// has to stay valid after a later shape is appended, and a vector's does
		// not.
		static std::deque<Shape> shapes;

		for (Shape &shape : shapes) {
			if (shape.Count == count && shape.Affinities == affinities && shape.Bands == bands) {
				return shape.Built;
			}
		}

		Shape shape{count, affinities, bands, {}};
		shape.Built.reserve(count);
		for (uint64_t index = 0; index < count; index++) {
			GroupCandidate candidate;
			candidate.Root = RootOf(index);
			candidate.Bytes = BytesOf(index);
			candidate.Affinity = affinities == 0 ? 0 : static_cast<uint32_t>(index % affinities) + 1;
			candidate.Priority = static_cast<uint32_t>(index % bands);
			shape.Built.push_back(candidate);
		}
		shapes.push_back(std::move(shape));
		return shapes.back().Built;
	}

	// One assembly, with the group count consumed so the work cannot be elided.
	size_t Assemble(const Grouper &grouper, const std::vector<GroupCandidate> &candidates) {
		const Assembly assembly = grouper.Assemble(candidates);
		return assembly.Groups.size() + assembly.Oversized;
	}
}

using namespace grouping_bench;

// --- how it grows -------------------------------------------------------------
//
// Ten times the assets should be near ten times the cost. A hundred times is a
// relation being evaluated pairwise, and the input where that first hurts is a
// real game rather than a test.

BENCH("Assemble · 1k assets, nothing bound together", 1) {
	static const Grouper grouper;
	Consume(Assemble(grouper, Candidates(SMALL, 0, 1)));
}

BENCH("Assemble · 10k assets, nothing bound together", 1) {
	static const Grouper grouper;
	Consume(Assemble(grouper, Candidates(MEDIUM, 0, 1)));
}

BENCH("Assemble · 50k assets, nothing bound together", 1) {
	static const Grouper grouper;
	Consume(Assemble(grouper, Candidates(LARGE, 0, 1)));
}

// --- the shapes ---------------------------------------------------------------

BENCH("Assemble · 10k assets in 2k affinities", 1) {
	// **Rule 1, which outranks the size bound.** Five assets to an affinity is
	// what a mesh with its textures, its material and its collider looks like,
	// so this is the ordinary case rather than a stress one - and it is a
	// different path from the row above, because an affinity is gathered before
	// anything is weighed.
	static const Grouper grouper;
	Consume(Assemble(grouper, Candidates(MEDIUM, 2000, 1)));
}

BENCH("Assemble · 10k assets in 20 affinities", 1) {
	// Five hundred assets bound to each other. Every affinity is far past the
	// group maximum, so every group is oversized and the widening branch runs
	// for all of them - the case `Assembly::Oversized` exists to report rather
	// than tolerate silently.
	static const Grouper grouper;
	Consume(Assemble(grouper, Candidates(MEDIUM, 20, 1)));
}

BENCH("Assemble · 10k assets across 64 priority bands", 1) {
	// Rule 3. A group takes the lowest priority among its members, so bands
	// change which assets can share a group as well as what order the groups
	// come out in.
	static const Grouper grouper;
	Consume(Assemble(grouper, Candidates(MEDIUM, 2000, 64)));
}

// --- the envelope -------------------------------------------------------------
//
// Same assets, different bound. CDN.md §9 carries the group size as an open
// question with no number beside it; these are what the question costs.

BENCH("Assemble · 10k assets into 4 MiB groups", 1) {
	static const Grouper grouper(
		GroupPolicy{.TargetBytes = 4ull * 1024 * 1024, .MaximumBytes = 8ull * 1024 * 1024}
	);
	Consume(Assemble(grouper, Candidates(MEDIUM, 2000, 1)));
}

BENCH("Assemble · 10k assets into 64 MiB groups", 1) {
	static const Grouper grouper(
		GroupPolicy{.TargetBytes = 64ull * 1024 * 1024, .MaximumBytes = 128ull * 1024 * 1024}
	);
	Consume(Assemble(grouper, Candidates(MEDIUM, 2000, 1)));
}
