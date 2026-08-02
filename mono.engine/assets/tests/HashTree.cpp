#include <engine/assets/HashTree.hpp>
#include <engine/core/FrameGraph.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.assets.hashtree")
TEST_DEPENDS("engine.assets.contenthash")
TEST_DEPENDS("engine.core.framegraph")
TEST_DEPENDS("engine.core.metrics")

using engine::assets::ContentHash;
using engine::assets::Hasher;
using engine::assets::HashTree;
using engine::core::FrameGraph;
using engine::core::Metrics;

namespace {
	// Leaf `index` of a tree, named so that a failure says which one.
	ContentHash Leaf(size_t index) {
		const std::string text = "leaf-" + std::to_string(index);
		return Hasher::Of(
			std::span<const std::byte>(reinterpret_cast<const std::byte *>(text.data()), text.size())
		);
	}

	std::vector<ContentHash> Leaves(size_t count) {
		std::vector<ContentHash> leaves;
		leaves.reserve(count);
		for (size_t index = 0; index < count; ++index) {
			leaves.push_back(Leaf(index));
		}
		return leaves;
	}
}

TEST_CASE("every leaf proves against the root", "[assets][hashtree]") {
	// Odd and even counts, powers of two and the awkward sizes between them —
	// promotion of the odd node out only happens at some of these, and it is
	// the case a tree implementation gets wrong.
	for (const size_t count : {1u, 2u, 3u, 4u, 5u, 7u, 8u, 9u, 16u, 17u, 100u, 129u}) {
		const auto leaves = Leaves(count);
		const HashTree tree = HashTree::Build(leaves);

		INFO("leaf count " << count);
		REQUIRE(tree.LeafCount() == count);

		for (size_t index = 0; index < count; ++index) {
			INFO("leaf " << index);
			const auto proof = tree.Proof(index);
			CHECK(HashTree::Verify(leaves[index], index, count, proof, tree.Root()));
		}
	}
}

TEST_CASE("a wrong leaf does not verify", "[assets][hashtree]") {
	const auto leaves = Leaves(9);
	const HashTree tree = HashTree::Build(leaves);
	const auto proof = tree.Proof(3);

	CHECK(HashTree::Verify(leaves[3], 3, 9, proof, tree.Root()));

	// The whole point: a client holding a root will not accept bytes that do
	// not hash into it. CDN.md §1 — a compromised origin may withhold content,
	// never substitute it.
	CHECK_FALSE(HashTree::Verify(Leaf(999), 3, 9, proof, tree.Root()));
	CHECK_FALSE(HashTree::Verify(leaves[4], 3, 9, proof, tree.Root()));
}

TEST_CASE("a leaf does not verify at the wrong index", "[assets][hashtree]") {
	const auto leaves = Leaves(8);
	const HashTree tree = HashTree::Build(leaves);

	// Position is part of what is proven. Otherwise a chunk could be replayed
	// into a different offset of the same asset.
	CHECK_FALSE(HashTree::Verify(leaves[2], 5, 8, tree.Proof(2), tree.Root()));
	CHECK_FALSE(HashTree::Verify(leaves[2], 2, 8, tree.Proof(5), tree.Root()));
}

TEST_CASE("a tampered proof does not verify", "[assets][hashtree]") {
	const auto leaves = Leaves(16);
	const HashTree tree = HashTree::Build(leaves);

	auto proof = tree.Proof(6);
	REQUIRE_FALSE(proof.empty());

	proof[0].Digest[0] ^= 0x01;
	CHECK_FALSE(HashTree::Verify(leaves[6], 6, 16, proof, tree.Root()));
}

TEST_CASE("a truncated or padded proof does not verify", "[assets][hashtree]") {
	const auto leaves = Leaves(16);
	const HashTree tree = HashTree::Build(leaves);
	const auto proof = tree.Proof(6);
	REQUIRE(proof.size() > 1);

	// Short: a missing sibling must not be mistaken for a promoted subtree.
	auto shortened = proof;
	shortened.pop_back();
	CHECK_FALSE(HashTree::Verify(leaves[6], 6, 16, shortened, tree.Root()));

	// Long: leftover entries mean the path does not describe this tree, and
	// accepting them would let one proof be padded into another.
	auto lengthened = proof;
	lengthened.push_back(Leaf(1234));
	CHECK_FALSE(HashTree::Verify(leaves[6], 6, 16, lengthened, tree.Root()));
}

TEST_CASE("the leaf count is part of what the root commits to", "[assets][hashtree]") {
	const auto leaves = Leaves(8);
	const HashTree tree = HashTree::Build(leaves);

	// Claiming a different shape fails even with a genuine leaf and a genuine
	// path. This is what makes a subtree unusable as a whole tree — the hole
	// that promotion of an odd node would otherwise leave open.
	CHECK_FALSE(HashTree::Verify(leaves[0], 0, 7, tree.Proof(0), tree.Root()));
	CHECK_FALSE(HashTree::Verify(leaves[0], 0, 9, tree.Proof(0), tree.Root()));
	CHECK_FALSE(HashTree::Verify(leaves[0], 0, 0, tree.Proof(0), tree.Root()));
}

TEST_CASE("trees of different sizes have different roots", "[assets][hashtree]") {
	// Three leaves promotes the odd one out; four does not. Without the leaf
	// count sealed into the root these two can be made to agree, which is the
	// classic duplicate-node flaw.
	const auto three = Leaves(3);
	std::vector<ContentHash> duplicated = three;
	duplicated.push_back(three[2]);

	CHECK(HashTree::RootOf(three) != HashTree::RootOf(duplicated));
}

TEST_CASE("an interior node cannot be passed off as a leaf", "[assets][hashtree]") {
	const auto leaves = Leaves(4);

	// Domain separation. The interior combination is prefixed, so the hash of a
	// pair is not something a chunk could ever hash to by construction.
	const ContentHash interior = HashTree::CombineNodes(leaves[0], leaves[1]);

	Hasher plain;
	plain.Update(
		std::span<const std::byte>(
			reinterpret_cast<const std::byte *>(leaves[0].Digest.data()), ContentHash::BYTES
		)
	);
	plain.Update(
		std::span<const std::byte>(
			reinterpret_cast<const std::byte *>(leaves[1].Digest.data()), ContentHash::BYTES
		)
	);

	CHECK(interior != plain.Finish());
}

TEST_CASE("combination is ordered", "[assets][hashtree]") {
	const auto leaves = Leaves(2);

	// Left and right are not interchangeable, or a proof would verify with its
	// siblings in either position and the index would stop meaning anything.
	CHECK(HashTree::CombineNodes(leaves[0], leaves[1]) != HashTree::CombineNodes(leaves[1], leaves[0]));
}

TEST_CASE("a single leaf needs no proof", "[assets][hashtree]") {
	const auto leaves = Leaves(1);
	const HashTree tree = HashTree::Build(leaves);

	CHECK(tree.Proof(0).empty());
	CHECK(HashTree::Verify(leaves[0], 0, 1, {}, tree.Root()));

	// And the root is not simply the leaf: the count is sealed in, so a lone
	// chunk's address and the asset that holds only it stay distinguishable.
	CHECK(tree.Root() != leaves[0]);
}

TEST_CASE("an empty tree has a root and no valid leaves", "[assets][hashtree]") {
	const HashTree tree = HashTree::Build({});

	CHECK(tree.LeafCount() == 0);
	CHECK_FALSE(tree.Root().IsZero());
	CHECK(tree.Proof(0).empty());

	// An asset with no chunks and an asset whose chunks are unknown are
	// different values, which is what a caller needs from this.
	CHECK(tree.Root() != HashTree::RootOf(Leaves(1)));
	CHECK_FALSE(HashTree::Verify(ContentHash{}, 0, 0, {}, tree.Root()));
}

TEST_CASE("RootOf agrees with Build", "[assets][hashtree]") {
	for (const size_t count : {0u, 1u, 5u, 8u, 33u}) {
		const auto leaves = Leaves(count);
		INFO("leaf count " << count);
		CHECK(HashTree::RootOf(leaves) == HashTree::Build(leaves).Root());
	}
}

TEST_CASE("one changed leaf changes the root", "[assets][hashtree]") {
	auto leaves = Leaves(64);
	const ContentHash before = HashTree::RootOf(leaves);

	// The invalidation property: one edited chunk changes its asset root, and
	// that chain is what an edge cache is told to drop.
	leaves[37] = Leaf(1'000'000);
	CHECK(HashTree::RootOf(leaves) != before);
}

TEST_CASE("reordering leaves changes the root", "[assets][hashtree]") {
	auto leaves = Leaves(10);
	const ContentHash before = HashTree::RootOf(leaves);

	std::swap(leaves[2], leaves[7]);
	CHECK(HashTree::RootOf(leaves) != before);
}

TEST_CASE(
	"verification reports itself to the frame graph and the metrics sink", "[assets][hashtree][framegraph]"
) {
	const auto leaves = Leaves(8);
	const HashTree tree = HashTree::Build(leaves);
	const auto proof = tree.Proof(1);

	Metrics::Clear();
	FrameGraph::SetEnabled(true);
	FrameGraph::BeginFrame();
	const HashTree built = HashTree::Build(leaves);
	CHECK(HashTree::Verify(leaves[1], 1, 8, proof, built.Root()));
	CHECK_FALSE(HashTree::Verify(Leaf(999), 1, 8, proof, built.Root()));
	FrameGraph::EndFrame();
	const std::vector<engine::core::FrameSpan> spans(FrameGraph::Spans().begin(), FrameGraph::Spans().end());
	FrameGraph::SetEnabled(false);

	const auto named = [&spans](std::string_view name) {
		return std::any_of(spans.begin(), spans.end(), [name](const auto &span) {
			return span.Name == name;
		});
	};

	CHECK(named("HashTree::Build"));
	CHECK(named("HashTree::Verify"));

	const auto counters = Metrics::Drain();
	const auto total = [&counters](std::string_view name) {
		double sum = 0.0;
		for (const auto &counter : counters) {
			if (counter.Name == engine::core::Name(name)) {
				sum += counter.Value;
			}
		}
		return sum;
	};

	// Passed and failed counted apart. A failure rate that climbs is somebody
	// serving bytes that do not match a root the client trusts, and that reads
	// nothing like content simply being absent.
	CHECK(total("assets.verify.passed") == 1.0);
	CHECK(total("assets.verify.failed") == 1.0);
}
