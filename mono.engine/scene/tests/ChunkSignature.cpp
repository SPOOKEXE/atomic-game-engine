// Signing a draw list in chunks, so a consumer can tell *where* it moved.
//
// `SignatureOf` answers "did anything change" and its cases live in
// `tests/DrawInstance.cpp`. These are about the property that one buys nothing
// and the other is built for: that a change is confined to the chunk holding it,
// so the count of dirty chunks means something.

#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <span>
#include <vector>

TEST_SUITE_ID("engine.scene.chunksignature")

using engine::core::CFrame;
using engine::core::Name;
using engine::core::Vector3;
using engine::scene::ChunkSignaturesOf;
using engine::scene::DirtyChunkCount;
using engine::scene::DrawInstance;
using engine::scene::SIGNATURE_CHUNK;

namespace {
	// A list of `count` instances, each placed somewhere different so no two
	// chunks are accidentally equal.
	std::vector<DrawInstance> ListOf(size_t count) {
		std::vector<DrawInstance> instances(count);
		for (size_t index = 0; index < count; index++) {
			const auto at = static_cast<float>(index);
			instances[index].Frame = CFrame(Vector3(at, at * 0.5f, -at));
		}
		return instances;
	}

	// The identity permutation, for a case that wants an explicit order rather
	// than the empty-means-identity shorthand.
	std::vector<uint32_t> IdentityOrder(size_t count) {
		std::vector<uint32_t> order(count);
		std::iota(order.begin(), order.end(), 0u);
		return order;
	}

	std::vector<uint64_t> SignaturesOf(const std::vector<DrawInstance> &instances) {
		std::vector<uint64_t> chunks;
		ChunkSignaturesOf(instances, {}, chunks);
		return chunks;
	}
}

TEST_CASE("a list is signed in whole chunks with a partial tail", "[scene][chunksignature]") {
	CHECK(SignaturesOf(ListOf(0)).empty());
	CHECK(SignaturesOf(ListOf(1)).size() == 1);
	CHECK(SignaturesOf(ListOf(SIGNATURE_CHUNK)).size() == 1);
	CHECK(SignaturesOf(ListOf(SIGNATURE_CHUNK + 1)).size() == 2);
	CHECK(SignaturesOf(ListOf(SIGNATURE_CHUNK * 4)).size() == 4);
}

TEST_CASE("an empty order means the list is signed as it stands", "[scene][chunksignature]") {
	// The shorthand every caller with no permutation uses, and it must agree
	// with spelling the identity out - otherwise the two kinds of caller would
	// produce signatures that never compare equal.
	const std::vector<DrawInstance> instances = ListOf(SIGNATURE_CHUNK * 2 + 7);

	std::vector<uint64_t> implicit;
	ChunkSignaturesOf(instances, {}, implicit);

	const std::vector<uint32_t> order = IdentityOrder(instances.size());
	std::vector<uint64_t> explicitOrder;
	ChunkSignaturesOf(instances, order, explicitOrder);

	CHECK(implicit == explicitOrder);
}

TEST_CASE("one instance moving dirties one chunk", "[scene][chunksignature]") {
	// **The property the whole measurement rests on.** A running hash carried
	// across chunk boundaries would make every chunk after the moved instance
	// change too, so a single part twitching at the head of a list would report
	// the entire scene as dirty - which is exactly the reading that would send
	// somebody off to build a delta upload that could never save anything.
	std::vector<DrawInstance> instances = ListOf(SIGNATURE_CHUNK * 4);
	const std::vector<uint64_t> before = SignaturesOf(instances);

	// Something in the second chunk.
	instances[SIGNATURE_CHUNK + 10].Frame = CFrame(Vector3(999.0f, 0.0f, 0.0f));
	const std::vector<uint64_t> after = SignaturesOf(instances);

	CHECK(DirtyChunkCount(before, after) == 1);
	CHECK(before[0] == after[0]);
	CHECK(before[1] != after[1]);
	CHECK(before[2] == after[2]);
	CHECK(before[3] == after[3]);
}

TEST_CASE("a still list dirties nothing", "[scene][chunksignature]") {
	// A scene where nothing moved must read as zero dirty chunks on every frame,
	// not merely on most of them. A signature that drifted on its own would not
	// fail here loudly - it would report a still world as fully dirty and look
	// exactly like a world that was moving.
	const std::vector<DrawInstance> instances = ListOf(SIGNATURE_CHUNK * 3 + 40);
	const std::vector<uint64_t> first = SignaturesOf(instances);
	const std::vector<uint64_t> second = SignaturesOf(instances);

	CHECK(first == second);
	CHECK(DirtyChunkCount(first, second) == 0);

	// And a separately built list holding the same values signs the same, for
	// `SignatureOf`'s reason: equality is over what a list says, not over where
	// it lives.
	const std::vector<DrawInstance> twin(instances.begin(), instances.end());
	CHECK(DirtyChunkCount(first, SignaturesOf(twin)) == 0);
}

TEST_CASE("a resort dirties the rows it moved, not the instances", "[scene][chunksignature]") {
	// **Why `order` is a parameter rather than something a caller applies
	// first.** The renderer uploads `instances[order[i]]` at row `i`, so a frame
	// that merely re-sorted the same untouched instances has genuinely changed
	// the bytes at those rows. A signature taken over the list before the
	// permutation would call that frame clean, and a delta built on it would
	// leave the previous frame's sort on screen - which for the blended pass is
	// transparency drawn in the wrong order.
	const std::vector<DrawInstance> instances = ListOf(SIGNATURE_CHUNK * 2);

	std::vector<uint32_t> order = IdentityOrder(instances.size());
	std::vector<uint64_t> before;
	ChunkSignaturesOf(instances, order, before);

	// Reverse the first chunk only. No instance changed; a quarter of the rows
	// did.
	std::reverse(order.begin(), order.begin() + SIGNATURE_CHUNK);
	std::vector<uint64_t> after;
	ChunkSignaturesOf(instances, order, after);

	CHECK(before[0] != after[0]);
	CHECK(before[1] == after[1]);
	CHECK(DirtyChunkCount(before, after) == 1);
}

TEST_CASE("a chunk's signature depends on the order inside it", "[scene][chunksignature]") {
	// Two instances swapped within one chunk must change that chunk. The lanes
	// in `FoldInstance` are independent accumulators, and a fold that was
	// order-insensitive - a sum, an xor - would report a swap as clean.
	std::vector<DrawInstance> instances = ListOf(4);
	const std::vector<uint64_t> before = SignaturesOf(instances);

	std::swap(instances[0], instances[2]);
	CHECK(SignaturesOf(instances) != before);
}

TEST_CASE("a list that grew or shrank counts its new chunks dirty", "[scene][chunksignature]") {
	// **Length is work, not a comparison.** Rows past the end of the shorter
	// list have either never been uploaded or must stop being drawn, and a
	// counter that called them clean would report a world streaming in as
	// costing nothing - the one frame where the number most needs to be honest.
	const std::vector<uint64_t> small = SignaturesOf(ListOf(SIGNATURE_CHUNK * 2));
	const std::vector<uint64_t> large = SignaturesOf(ListOf(SIGNATURE_CHUNK * 5));

	CHECK(DirtyChunkCount(small, large) == 3);
	CHECK(DirtyChunkCount(large, small) == 3);

	// The first frame of all: nothing to compare against, so everything is new.
	const std::span<const uint64_t> nothing;
	CHECK(DirtyChunkCount(nothing, large) == 5);
	CHECK(DirtyChunkCount(nothing, nothing) == 0);
}

TEST_CASE("a shorter order signs only the rows it names", "[scene][chunksignature]") {
	// A culled view uploads a subset of the world's draw list, so the row count
	// is the permutation's length and not the list's. Signing the whole list
	// would make a view's dirty count move with instances that view never draws.
	const std::vector<DrawInstance> instances = ListOf(SIGNATURE_CHUNK * 4);

	std::vector<uint32_t> visible = IdentityOrder(SIGNATURE_CHUNK);
	std::vector<uint64_t> chunks;
	CHECK(ChunkSignaturesOf(instances, visible, chunks) == 1);
	CHECK(chunks.size() == 1);
}

TEST_CASE("every field that moves a signature moves its chunk", "[scene][chunksignature]") {
	// **The fold is shared with `SignatureOf` and this is the case that keeps it
	// shared.** If the two ever grew separate copies, the one that would fall
	// behind is this one - it is read by a counter rather than by a picture, so a
	// field left out of it reports a scene as quieter than it is and nothing
	// looks wrong on screen.
	const std::vector<DrawInstance> base(1);
	const uint64_t unchanged = SignaturesOf(base)[0];

	auto moved = [&](auto edit) {
		std::vector<DrawInstance> instances(1);
		edit(instances[0]);
		return SignaturesOf(instances)[0];
	};

	CHECK(moved([](DrawInstance &i) { i.Frame = CFrame(Vector3(0.0f, 0.0f, 1.0f)); }) != unchanged);
	CHECK(moved([](DrawInstance &i) { i.HalfExtent = Vector3(2.0f, 1.0f, 1.0f); }) != unchanged);
	CHECK(moved([](DrawInstance &i) { i.Transparency = 0.5f; }) != unchanged);
	CHECK(moved([](DrawInstance &i) { i.Mesh = Name("chunksignature_test.Pane"); }) != unchanged);
	CHECK(moved([](DrawInstance &i) { i.Texture = Name("chunksignature_test.Brick"); }) != unchanged);
	CHECK(moved([](DrawInstance &i) { i.Surface = 2; }) != unchanged);
	CHECK(moved([](DrawInstance &i) { i.SeamOffset = 0.25f; }) != unchanged);
}

TEST_CASE("the chunk boundary falls exactly where the arithmetic says", "[scene][chunksignature]") {
	// **An off-by-one here does not fail loudly, it makes the count wrong by a
	// chunk for ever.** The row at `SIGNATURE_CHUNK - 1` is the last row of the
	// first chunk and the row at `SIGNATURE_CHUNK` is the first of the second,
	// and a delta upload built on this would copy the wrong byte range - which
	// draws as one part of the scene rendering last frame's transform.
	const size_t rows = SIGNATURE_CHUNK * 2;

	std::vector<DrawInstance> last = ListOf(rows);
	last[SIGNATURE_CHUNK - 1].Transparency = 0.5f;

	std::vector<DrawInstance> first = ListOf(rows);
	first[SIGNATURE_CHUNK].Transparency = 0.5f;

	const std::vector<uint64_t> clean = SignaturesOf(ListOf(rows));
	const std::vector<uint64_t> lastOfFirst = SignaturesOf(last);
	const std::vector<uint64_t> firstOfSecond = SignaturesOf(first);

	CHECK(lastOfFirst[0] != clean[0]);
	CHECK(lastOfFirst[1] == clean[1]);

	CHECK(firstOfSecond[0] == clean[0]);
	CHECK(firstOfSecond[1] != clean[1]);
}

TEST_CASE("a change in the partial tail dirties the tail", "[scene][chunksignature]") {
	// The last chunk of a list is almost never full, and a loop that signed only
	// whole chunks would leave those rows unwatched - so the tail of every scene
	// would report clean no matter what happened in it. Real draw lists are
	// almost all tail: a scene of forty parts is one partial chunk and nothing
	// else.
	std::vector<DrawInstance> instances = ListOf(SIGNATURE_CHUNK + 5);
	const std::vector<uint64_t> before = SignaturesOf(instances);
	REQUIRE(before.size() == 2);

	instances.back().Tint = engine::core::Color3(0.1f, 0.2f, 0.3f);
	const std::vector<uint64_t> after = SignaturesOf(instances);

	CHECK(after[0] == before[0]);
	CHECK(after[1] != before[1]);
	CHECK(DirtyChunkCount(before, after) == 1);
}

TEST_CASE("an order naming a row past the list is survivable", "[scene][chunksignature]") {
	// A caller handing over an index past the end is a bug in that caller, and
	// this is not the place it should be found - but a signature is not worth
	// reading off the end of a vector for either. The row folds in nothing and
	// the chunk stays comparable, so a miscounted view reports a stale-looking
	// number rather than reading somebody else's memory.
	const std::vector<DrawInstance> instances = ListOf(4);
	const std::vector<uint32_t> order{0u, 1u, 99u, 3u};

	std::vector<uint64_t> chunks;
	CHECK(ChunkSignaturesOf(instances, order, chunks) == 1);
	CHECK(chunks.size() == 1);

	// And it is stable, which is the property that keeps the dirty count from
	// pinning itself at "everything" while the bug is live.
	std::vector<uint64_t> again;
	ChunkSignaturesOf(instances, order, again);
	CHECK(chunks == again);
}

TEST_CASE("the vector handed in is reused rather than grown", "[scene][chunksignature]") {
	// The renderer keeps one of these per scene slot and hands it back every
	// frame, so `ChunkSignaturesOf` has to resize down as well as up. A function
	// that only ever appended would make a scene that shrank compare against
	// chunks that no longer exist and report them dirty for ever.
	std::vector<uint64_t> chunks;
	ChunkSignaturesOf(ListOf(SIGNATURE_CHUNK * 4), {}, chunks);
	REQUIRE(chunks.size() == 4);

	ChunkSignaturesOf(ListOf(SIGNATURE_CHUNK), {}, chunks);
	CHECK(chunks.size() == 1);

	ChunkSignaturesOf({}, {}, chunks);
	CHECK(chunks.empty());
}

TEST_CASE("one row entering the list dirties every chunk after it", "[scene][chunksignature]") {
	// **The finding that decides whether a delta upload is worth building, and
	// it is a negative one.** A chunk is a range of *row indices*, and the rows
	// are `instances[order[i]]` - so an instance appearing or disappearing at the
	// head shifts every row behind it by one and changes every chunk from there
	// to the end, even though not one of those instances moved.
	//
	// That is not a rare case. `render::ViewRecording` signs the culled list, so
	// a single part crossing the frustum edge does this, and a camera in motion
	// does it most frames. `MeshGrid.luau` does it by growing the list on
	// `Heartbeat` as meshes stream in - measured at 100% of chunks rewritten
	// while its geometry sat still.
	//
	// The fix is not a better hash. It is to key the resident rows by a stable
	// per-entity slot, the way `replication::Authority` keys a component, and to
	// make visibility and draw order a separate index list the GPU reads - which
	// is what `ROADMAP.md`'s v0.19 entry means by "replication to the GPU", and
	// what the occlusion cull already does in miniature when it compacts
	// survivors. Until rows are keyed that way, a delta has nothing stable to be
	// a delta against.
	const std::vector<DrawInstance> world = ListOf(SIGNATURE_CHUNK * 4);

	std::vector<uint32_t> all = IdentityOrder(world.size());
	std::vector<uint64_t> before;
	ChunkSignaturesOf(world, all, before);
	REQUIRE(before.size() == 4);

	// One instance near the front leaves the frustum. Nothing else changed.
	std::vector<uint32_t> culled = all;
	culled.erase(culled.begin() + 3);
	std::vector<uint64_t> after;
	ChunkSignaturesOf(world, culled, after);

	// Every chunk, not one. The last is short by a row as well, which is why the
	// count is four rather than three.
	CHECK(DirtyChunkCount(before, after) == 4);

	// **The same list re-sorted only in its tail leaves the head alone**, which
	// is the half that does work: opaque rows are ordered in world order and only
	// the blended run follows the eye, so a camera turn over static opaque
	// geometry does not have this problem. It is the *membership* changing that
	// does.
	std::vector<uint32_t> resortedTail = all;
	std::reverse(resortedTail.begin() + SIGNATURE_CHUNK * 3, resortedTail.end());
	std::vector<uint64_t> tail;
	ChunkSignaturesOf(world, resortedTail, tail);

	CHECK(DirtyChunkCount(before, tail) == 1);
	CHECK(tail[0] == before[0]);
	CHECK(tail[3] != before[3]);
}
