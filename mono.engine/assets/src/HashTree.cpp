#include <engine/assets/HashTree.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>

#include <array>

namespace engine::assets {

	namespace {
		// Domain separation. Every hash in the tree is prefixed with what kind
		// of thing it is, so that a digest produced for one role can never be
		// read as one produced for another.
		//
		// Without the interior prefix, an attacker holding a subtree root could
		// present it as a *chunk* hash: the verifier would combine the same
		// bytes the same way and get the same answer. With it, doing so needs a
		// chunk whose plain BLAKE3 equals an 0x01-prefixed digest, which is a
		// preimage problem rather than a bookkeeping one.
		//
		// Leaves carry no prefix, and that is deliberate rather than an
		// oversight: a chunk's address must be the hash of its content and
		// nothing else, or two systems that agree on the bytes disagree on the
		// name and dedup stops working across them.
		constexpr std::byte INTERIOR_TAG{0x01};
		constexpr std::byte ROOT_TAG{0x02};

		std::span<const std::byte> AsBytes(const ContentHash &hash) {
			return std::span<const std::byte>(
				reinterpret_cast<const std::byte *>(hash.Digest.data()), ContentHash::BYTES
			);
		}

		// Binds the leaf count into the root.
		//
		// This is what makes the tree's *shape* part of what a root commits to.
		// Odd levels promote their last node unchanged, which is the usual
		// answer and the usual hole: without the count, a tree of three leaves
		// and one of four can be arranged to share a top node, and a subtree can
		// be presented as a whole tree. Hashing the count in closes both, and
		// costs one compression function per tree.
		ContentHash SealRoot(size_t leafCount, const ContentHash &top) {
			std::array<std::byte, 8> count{};
			uint64_t value = static_cast<uint64_t>(leafCount);
			for (size_t index = 0; index < count.size(); ++index) {
				count[index] = static_cast<std::byte>(value & 0xFF);
				value >>= 8;
			}

			Hasher hasher;
			hasher.Update(std::span<const std::byte>(&ROOT_TAG, 1));
			hasher.Update(count);
			hasher.Update(AsBytes(top));
			return hasher.Finish();
		}
	}

	ContentHash HashTree::CombineNodes(const ContentHash &left, const ContentHash &right) {
		Hasher hasher;
		hasher.Update(std::span<const std::byte>(&INTERIOR_TAG, 1));
		hasher.Update(AsBytes(left));
		hasher.Update(AsBytes(right));
		return hasher.Finish();
	}

	HashTree HashTree::Build(std::span<const ContentHash> leaves) {
		ENGINE_PROFILE_CAT("HashTree::Build", core::ProfileCategory::Assets);

		HashTree tree;
		tree.Leaves = leaves.size();

		if (leaves.empty()) {
			// An empty tree still has a root, and it is the sealed hash of a
			// zero count over a zero top. No special case anywhere else: an
			// asset with no chunks and an asset whose chunks are unknown are
			// then different values, which is what a caller needs.
			tree.TopHash = SealRoot(0, ContentHash{});
			return tree;
		}

		tree.Levels.emplace_back(leaves.begin(), leaves.end());

		while (tree.Levels.back().size() > 1) {
			const std::vector<ContentHash> &below = tree.Levels.back();
			std::vector<ContentHash> above;
			above.reserve((below.size() + 1) / 2);

			size_t index = 0;
			for (; index + 1 < below.size(); index += 2) {
				above.push_back(CombineNodes(below[index], below[index + 1]));
			}
			if (index < below.size()) {
				// Odd node out. Promoted unchanged rather than paired with a
				// copy of itself - duplicating it makes two different leaf sets
				// produce one root, which is the flaw the leaf count in
				// SealRoot also guards, and there is no reason to rely on only
				// one of the two.
				above.push_back(below[index]);
			}

			tree.Levels.push_back(std::move(above));
		}

		tree.TopHash = SealRoot(tree.Leaves, tree.Levels.back().front());
		return tree;
	}

	ContentHash HashTree::RootOf(std::span<const ContentHash> leaves) {
		return Build(leaves).Root();
	}

	std::vector<ContentHash> HashTree::Proof(size_t index) const {
		std::vector<ContentHash> path;
		if (index >= Leaves || Levels.empty()) {
			return path;
		}

		size_t position = index;
		for (size_t level = 0; level + 1 < Levels.size(); ++level) {
			const std::vector<ContentHash> &nodes = Levels[level];
			const size_t sibling = position ^ 1;
			if (sibling < nodes.size()) {
				path.push_back(nodes[sibling]);
			}
			// A promoted node contributes nothing, and the verifier works out
			// the same gap from the leaf count rather than from a marker in the
			// path. A marker would be a second encoding of a fact the count
			// already carries.
			position /= 2;
		}

		return path;
	}

	// Counted here rather than at each `return false`, for the reason the
	// resolver in mono.cdn splits the same way: a refusal path added later is a
	// refusal path that forgets to count itself.
	bool HashTree::Verify(
		const ContentHash &leaf,
		size_t index,
		size_t leafCount,
		std::span<const ContentHash> proof,
		const ContentHash &root
	) {
		ENGINE_PROFILE_CAT("HashTree::Verify", core::ProfileCategory::Assets);

		const bool passed = Check(leaf, index, leafCount, proof, root);

		// A verification failure is a security signal and not a miss. A rate
		// that climbs means somebody is serving bytes that do not match a root
		// the client trusts - which is the one thing a compromised origin
		// cannot do without being seen. It is seen here.
		core::Metrics::Count(passed ? "assets.verify.passed" : "assets.verify.failed", 1.0);
		return passed;
	}

	bool HashTree::Check(
		const ContentHash &leaf,
		size_t index,
		size_t leafCount,
		std::span<const ContentHash> proof,
		const ContentHash &root
	) {
		if (leafCount == 0 || index >= leafCount) {
			return false;
		}

		ContentHash current = leaf;
		size_t position = index;
		size_t width = leafCount;
		size_t consumed = 0;

		while (width > 1) {
			const size_t sibling = position ^ 1;
			if (sibling < width) {
				if (consumed >= proof.size()) {
					// The path is shorter than the shape requires. Refuse rather
					// than treat a missing sibling as a promotion - that would
					// let a truncated proof verify.
					return false;
				}
				const ContentHash &other = proof[consumed++];
				current = (position % 2 == 0) ? CombineNodes(current, other) : CombineNodes(other, current);
			}
			// else: this subtree was promoted, and consumes no path entry.

			position /= 2;
			width = (width + 1) / 2;
		}

		// Every entry has to be used. A path with something left over is a path
		// that does not describe this tree, and accepting it would let one proof
		// be padded into another.
		if (consumed != proof.size()) {
			return false;
		}

		return SealRoot(leafCount, current) == root;
	}
}
