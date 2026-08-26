#pragma once

// The hierarchical hash - one root standing for many chunks.
//
// A flat list of chunk hashes would be enough to *store* an asset. It is not
// enough to deliver one, and delivery turns on four properties a tree has and
// a list does not:
//
// - **Verify while streaming.** A client checks chunk *k* against the root with
//   a path of siblings, as the chunk lands. With a list it must buffer the whole
//   asset before it can check anything, or check nothing and trust the
//   transport. A 400 MB mesh is the case that settles it.
// - **Patch for free.** Two versions are diffed by walking both trees top-down
//   and stopping wherever a subtree hash matches. What falls out is exactly the
//   set of chunks that changed - no patch format, and no second code path that
//   can disagree with the first.
// - **Invalidation is the changed path.** One edited chunk changes its asset
//   root, its bundle root and the manifest root. That chain *is* the set an edge
//   cache must drop.
// - **Dedup.** Two assets sharing a texture share its chunks.
//
// The same structure serves all four levels: chunks under an asset, assets
// under a bundle, bundles under the manifest. One implementation, three uses.
//
// @tier L8 · shared

#include <engine/assets/ContentHash.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::assets {

	// A binary Merkle tree over content hashes.
	//
	// Build it from the leaves, keep the root, hand out proofs. Verification
	// needs no tree at all - only the leaf, its index, the leaf count, the
	// sibling path and the root - which is what lets a client verify against a
	// root it was given without ever holding the whole structure.
	class HashTree {
	  public:
		// Builds the tree over `leaves`, in the order given.
		//
		// Order is significant and is the caller's to fix. Chunks are in stream
		// order because that is what an offset means; assets and bundles are
		// sorted by hash, because a manifest that differs run to run cannot be
		// diffed or cached.
		//
		// @param leaves The leaf hashes, in canonical order.
		// @return The tree.
		static HashTree Build(std::span<const ContentHash> leaves);

		// The root. The one value that has to be signed, stored or compared.
		const ContentHash &Root() const {
			return TopHash;
		}

		// How many leaves the tree was built over.
		size_t LeafCount() const {
			return Leaves;
		}

		// The sibling hashes proving that leaf `index` sits under Root().
		//
		// Bottom-up: the first entry is the leaf's sibling, the last is the
		// sibling nearest the root. A level where this leaf's subtree has no
		// sibling contributes nothing - Verify knows to expect that from the
		// leaf count, so the path carries no padding and no marker.
		//
		// @param index Zero-based leaf index.
		// @return The path, or empty when `index` is out of range or the tree
		//         holds one leaf and needs no path at all.
		std::vector<ContentHash> Proof(size_t index) const;

		// Whether `leaf` really is leaf `index` of a tree of `leafCount` leaves
		// whose root is `root`.
		//
		// Takes no HashTree, on purpose. A verifier holds a root it trusts and a
		// chunk that just arrived, and nothing else - requiring the tree would
		// mean shipping the structure it is meant to make unnecessary.
		//
		// @param leaf The hash being checked.
		// @param index Its zero-based position.
		// @param leafCount How many leaves the tree has. Part of what is proven:
		//        it pins the tree's shape, so a subtree cannot be passed off as
		//        a whole tree.
		// @param proof The sibling path from Proof().
		// @param root The trusted root.
		// @return True only if the path reconstructs exactly `root`.
		static bool Verify(
			const ContentHash &leaf,
			size_t index,
			size_t leafCount,
			std::span<const ContentHash> proof,
			const ContentHash &root
		);

		// The root of a tree over `leaves`, without keeping the tree.
		//
		// For the levels that only ever need a root - a bundle over its assets,
		// the manifest over its bundles - where building the interior levels to
		// throw them away is the only other option.
		//
		// @param leaves The leaf hashes, in canonical order.
		// @return The root.
		static ContentHash RootOf(std::span<const ContentHash> leaves);

		// Combines two child hashes into their parent.
		//
		// Exposed because a streaming verifier walks the same combination
		// without a tree, and two implementations of it would be two definitions
		// of the format.
		//
		// @param left The left child.
		// @param right The right child.
		// @return The interior node's hash.
		static ContentHash CombineNodes(const ContentHash &left, const ContentHash &right);

	  private:
		// The decision Verify wraps, with no counting around it. Split so that
		// Verify has exactly one place recording what it decided.
		static bool Check(
			const ContentHash &leaf,
			size_t index,
			size_t leafCount,
			std::span<const ContentHash> proof,
			const ContentHash &root
		);

		// Level 0 is the leaves; the last level holds the single top node.
		std::vector<std::vector<ContentHash>> Levels;
		ContentHash TopHash;
		size_t Leaves = 0;
	};
}
