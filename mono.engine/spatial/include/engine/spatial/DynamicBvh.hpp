#pragma once

// A mutable bounding-volume hierarchy over a moving proxy set.
//
// `HashGrid` deliberately rebuilds in one count-then-fill pass. DynamicBvh is
// the separate structure for a set whose membership is stable and whose boxes
// usually remain inside a deliberately fat leaf box. Its proxy ids remain
// opaque caller values. In particular, an id is not a persistent identity:
// `Sync` accepts only the same positional proxy sequence and makes a caller
// rebuild when membership or order changes.
//
// @tier L6 · shared

#include <engine/spatial/HashGrid.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::spatial {

	// What the dynamic hierarchy retained after its last synchronisation.
	struct DynamicBvhStats {
		size_t ProxyCount = 0;
		size_t EscapedLeaves = 0;
		size_t RefittedLeaves = 0;
		size_t ReinsertedLeaves = 0;
		size_t Rebuilds = 0;
		size_t QualityRebuilds = 0;
		size_t CachedPairs = 0;
		size_t CacheNodesVisited = 0;
		bool PairCacheAvailable = true;
		size_t LiveBytes = 0;
		size_t RetainedBytes = 0;
		size_t Height = 0;
	};

	// What a sync would change, computed without mutating the hierarchy.
	struct DynamicBvhPreflight {
		bool Compatible = false;
		size_t EscapedLeaves = 0;
		const Proxy *Source = nullptr;
		uint64_t Generation = 0;
	};

	// An incrementally synchronised hierarchy with retained flat storage.
	//
	// Leaves hold tight proxies and fat pruning bounds. Queries always test the
	// tight proxy box before reporting it, so the fat margin changes work only.
	class DynamicBvh {
	  public:
		// The absolute padding placed around each tight leaf bound, in metres.
		static constexpr float FAT_MARGIN = 0.25f;

		// The largest escaped fraction that stays on the incremental path.
		static constexpr size_t MAXIMUM_INCREMENTAL_DENOMINATOR = 8;

		// Replaces the tree with `proxies` in the supplied deterministic order.
		void Rebuild(std::span<const Proxy> proxies);

		// Reports whether `Sync` can preserve the current topology.
		//
		// The comparison is positional by design. A physics proxy id can be an
		// array index, so treating it as a durable key would retain a leaf for a
		// different collider after a reorder.
		DynamicBvhPreflight Preflight(std::span<const Proxy> proxies) const;

		// Updates tight boxes and refreshes every escaped fat leaf. Small changes
		// refit existing topology; larger changes reinsert leaves. `preflight` must
		// come from this tree for this exact span. The source and generation token
		// reject stale or mismatched plans without another linear scan. Returns
		// false without changing the tree when that contract does not hold.
		bool Sync(std::span<const Proxy> proxies, const DynamicBvhPreflight &preflight);

		// Empties the tree while keeping all retained allocations.
		void Clear();

		size_t ProxyCount() const {
			return Proxies.size();
		}

		DynamicBvhStats Stats() const;

		// Visits every exact unordered leaf pair once. BroadPhase uses this
		// instead of issuing one root query for each moving proxy; its final radix
		// sort remains responsible for public pair order.
		template <class Visit> bool ForEachOverlappingPair(Visit &&visit) const {
			if (PairCacheAvailable) {
				for (uint64_t packed : CandidatePairs) {
					const Proxy &first = Proxies[static_cast<uint32_t>(packed >> 32)];
					const Proxy &second = Proxies[static_cast<uint32_t>(packed)];
					if (first.Bounds.Overlaps(second.Bounds) && !visit(first, second)) {
						return false;
					}
				}
				return true;
			}
			auto exactVisit = [&visit](const Proxy &first, const Proxy &second) {
				return !first.Bounds.Overlaps(second.Bounds) || visit(first, second);
			};
			return Root == INVALID_NODE || ForEachPairNodes(Root, Root, exactVisit);
		}

	  private:
		static constexpr uint32_t INVALID_NODE = UINT32_MAX;
		static constexpr float MAXIMUM_PREDICTED_EXTENSION = 1.0f;
		static constexpr size_t MAXIMUM_CACHED_PAIRS = 1'000'000;

		struct Node {
			core::AABB Bounds;
			uint32_t Parent = INVALID_NODE;
			uint32_t Left = INVALID_NODE;
			uint32_t Right = INVALID_NODE;
			uint32_t ProxyIndex = INVALID_NODE;
		};

		std::vector<Proxy> Proxies;
		std::vector<Node> Nodes;
		std::vector<uint32_t> LeafNodes;
		std::vector<uint32_t> BuildOrder;
		std::vector<uint32_t> BuildKeys;
		std::vector<uint32_t> FreeNodes;
		std::vector<uint64_t> CandidatePairs;
		std::vector<uint64_t> PairScratch;
		std::vector<uint32_t> EscapedProxyIndices;
		std::vector<uint8_t> EscapedProxyFlags;
		uint32_t Root = INVALID_NODE;
		size_t EscapedLeaves = 0;
		size_t RefittedLeaves = 0;
		size_t ReinsertedLeaves = 0;
		size_t RebuildCount = 0;
		size_t QualityRebuildCount = 0;
		size_t CachedHeight = 0;
		size_t CacheNodesVisited = 0;
		bool PairCacheAvailable = true;
		uint64_t SyncGeneration = 0;

		uint32_t AllocateNode();
		void FreeNode(uint32_t node);
		void InsertLeaf(uint32_t leaf);
		void RemoveLeaf(uint32_t leaf);
		uint32_t BuildBalanced(size_t first, size_t last, uint32_t parent);
		void Refit(uint32_t node);
		bool Contains(const core::AABB &outer, const core::AABB &inner) const;
		core::AABB FatBounds(const core::AABB &bounds) const;
		core::AABB PredictedFatBounds(const core::AABB &previous, const core::AABB &current) const;
		size_t Height(uint32_t node) const;
		void PublishMetrics() const;
		void BuildPairCache();
		void UpdatePairCache();
		void AppendPairsForLeaf(uint32_t proxyIndex);

		template <class Visit> bool ForEachPairNodes(uint32_t left, uint32_t right, Visit &visit) const {
			const Node &a = Nodes[left];
			const Node &b = Nodes[right];
			if (!a.Bounds.Overlaps(b.Bounds)) {
				return true;
			}
			if (left == right) {
				if (a.ProxyIndex != INVALID_NODE) {
					return true;
				}
				return ForEachPairNodes(a.Left, a.Left, visit) && ForEachPairNodes(a.Left, a.Right, visit) &&
					   ForEachPairNodes(a.Right, a.Right, visit);
			}
			if (a.ProxyIndex != INVALID_NODE && b.ProxyIndex != INVALID_NODE) {
				const Proxy &first = Proxies[a.ProxyIndex];
				const Proxy &second = Proxies[b.ProxyIndex];
				return visit(first, second);
			}
			if (a.ProxyIndex != INVALID_NODE) {
				return ForEachPairNodes(left, b.Left, visit) && ForEachPairNodes(left, b.Right, visit);
			}
			if (b.ProxyIndex != INVALID_NODE) {
				return ForEachPairNodes(a.Left, right, visit) && ForEachPairNodes(a.Right, right, visit);
			}
			const auto surfaceArea = [](const core::AABB &bounds) {
				const core::Vector3 extent = bounds.Size();
				return 2.0f * (extent.X * extent.Y + extent.Y * extent.Z + extent.Z * extent.X);
			};
			const float leftArea = surfaceArea(a.Bounds);
			const float rightArea = surfaceArea(b.Bounds);
			if (leftArea > rightArea || (leftArea == rightArea && left < right)) {
				return ForEachPairNodes(a.Left, right, visit) && ForEachPairNodes(a.Right, right, visit);
			}
			return ForEachPairNodes(left, b.Left, visit) && ForEachPairNodes(left, b.Right, visit);
		}

		friend struct DynamicBvhInternals;
	};
}
