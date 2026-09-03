#include "RayBox.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/spatial/DynamicBvh.hpp>
#include <engine/spatial/Query.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <optional>

namespace engine::spatial {
	struct DynamicBvhInternals {
		template <class Visit>
		static bool
		ForEachCandidate(const DynamicBvh &tree, const core::AABB &volume, LayerMask mask, Visit &&visit) {
			if (tree.Root == DynamicBvh::INVALID_NODE) {
				return true;
			}

			// Parent links make this a stackless depth-first walk. A const query
			// therefore owns no visited state and does not allocate in proportion
			// to a malformed or deliberately skewed hierarchy.
			uint32_t at = tree.Root;
			uint32_t previous = DynamicBvh::INVALID_NODE;
			while (at != DynamicBvh::INVALID_NODE) {
				const DynamicBvh::Node &node = tree.Nodes[at];
				uint32_t next = node.Parent;
				if (previous == node.Parent && node.Bounds.Overlaps(volume)) {
					if (node.ProxyIndex == DynamicBvh::INVALID_NODE) {
						next = node.Left;
					} else {
						const Proxy &proxy = tree.Proxies[node.ProxyIndex];
						if (proxy.Layers.Overlaps(mask) && proxy.Bounds.Overlaps(volume) && !visit(proxy)) {
							return false;
						}
					}
				} else if (previous == node.Left) {
					next = node.Right;
				}
				previous = at;
				at = next;
			}
			return true;
		}
	};

	namespace {
		float SurfaceArea(const core::AABB &bounds) {
			const core::Vector3 extent = bounds.Size();
			return 2.0f * (extent.X * extent.Y + extent.Y * extent.Z + extent.Z * extent.X);
		}

		bool Finite(const core::AABB &bounds) {
			return std::isfinite(bounds.Minimum.X) && std::isfinite(bounds.Minimum.Y) &&
				   std::isfinite(bounds.Minimum.Z) && std::isfinite(bounds.Maximum.X) &&
				   std::isfinite(bounds.Maximum.Y) && std::isfinite(bounds.Maximum.Z);
		}

		bool DynamicCanTravel(const core::Ray &ray, float maxDistance) {
			return !(ray.Direction == core::Vector3::Zero) && maxDistance > 0.0f;
		}

		bool InsertId(std::span<uint64_t> found, uint64_t id, QueryResult &result) {
			if (result.Written == found.size()) {
				if (found.empty() || id >= found.back()) {
					result.Overflowed = true;
					return true;
				}
				result.Overflowed = true;
				result.Written--;
			}
			size_t slot = result.Written;
			while (slot > 0 && found[slot - 1] > id) {
				found[slot] = found[slot - 1];
				slot--;
			}
			found[slot] = id;
			result.Written++;
			return true;
		}

		core::AABB SegmentBounds(const core::Ray &ray, float distance) {
			const core::Vector3 end = ray.PointAt(distance);
			return core::AABB{
				core::Vector3{
					std::min(ray.Origin.X, end.X),
					std::min(ray.Origin.Y, end.Y),
					std::min(ray.Origin.Z, end.Z)
				},
				core::Vector3{
					std::max(ray.Origin.X, end.X),
					std::max(ray.Origin.Y, end.Y),
					std::max(ray.Origin.Z, end.Z)
				},
			};
		}

		void
		DynamicInsertNearest(std::span<core::RayHit> hits, const core::RayHit &hit, QueryResult &result) {
			if (result.Written == hits.size()) {
				if (hits.empty() || !(hit.Distance < hits.back().Distance ||
									  (hit.Distance == hits.back().Distance && hit.Id < hits.back().Id))) {
					result.Overflowed = true;
					return;
				}
				result.Overflowed = true;
				result.Written--;
			}
			size_t slot = result.Written;
			while (slot > 0 && (hits[slot - 1].Distance > hit.Distance ||
								(hits[slot - 1].Distance == hit.Distance && hits[slot - 1].Id > hit.Id))) {
				hits[slot] = hits[slot - 1];
				slot--;
			}
			hits[slot] = hit;
			result.Written++;
		}

	}

	uint32_t DynamicBvh::AllocateNode() {
		if (!FreeNodes.empty()) {
			const uint32_t node = FreeNodes.back();
			FreeNodes.pop_back();
			Nodes[node] = Node{};
			return node;
		}
		Nodes.emplace_back();
		return static_cast<uint32_t>(Nodes.size() - 1);
	}

	void DynamicBvh::FreeNode(uint32_t node) {
		Nodes[node] = Node{};
		FreeNodes.push_back(node);
	}

	core::AABB DynamicBvh::FatBounds(const core::AABB &bounds) const {
		if (!Finite(bounds)) {
			return bounds;
		}
		const core::Vector3 margin{FAT_MARGIN, FAT_MARGIN, FAT_MARGIN};
		return core::AABB{bounds.Minimum - margin, bounds.Maximum + margin};
	}

	core::AABB DynamicBvh::PredictedFatBounds(const core::AABB &previous, const core::AABB &current) const {
		if (!Finite(previous) || !Finite(current)) {
			return FatBounds(current);
		}
		core::AABB predicted = FatBounds(previous.Union(current));
		const core::Vector3 delta = current.Centre() - previous.Centre();
		const auto extension = [](float component) {
			return std::clamp(component * 2.0f, -MAXIMUM_PREDICTED_EXTENSION, MAXIMUM_PREDICTED_EXTENSION);
		};
		const core::Vector3 extra{extension(delta.X), extension(delta.Y), extension(delta.Z)};
		predicted.Minimum =
			predicted.Minimum +
			core::Vector3{std::min(extra.X, 0.0f), std::min(extra.Y, 0.0f), std::min(extra.Z, 0.0f)};
		predicted.Maximum =
			predicted.Maximum +
			core::Vector3{std::max(extra.X, 0.0f), std::max(extra.Y, 0.0f), std::max(extra.Z, 0.0f)};
		return predicted;
	}

	bool DynamicBvh::Contains(const core::AABB &outer, const core::AABB &inner) const {
		return outer.Contains(inner.Minimum) && outer.Contains(inner.Maximum);
	}

	void DynamicBvh::Refit(uint32_t node) {
		while (node != INVALID_NODE) {
			Node &current = Nodes[node];
			if (current.ProxyIndex == INVALID_NODE) {
				current.Bounds = Nodes[current.Left].Bounds.Union(Nodes[current.Right].Bounds);
			}
			node = current.Parent;
		}
	}

	void DynamicBvh::InsertLeaf(uint32_t leaf) {
		if (Root == INVALID_NODE) {
			Root = leaf;
			Nodes[leaf].Parent = INVALID_NODE;
			return;
		}

		uint32_t sibling = Root;
		while (Nodes[sibling].ProxyIndex == INVALID_NODE) {
			const Node &node = Nodes[sibling];
			const float leftCost = SurfaceArea(Nodes[node.Left].Bounds.Union(Nodes[leaf].Bounds));
			const float rightCost = SurfaceArea(Nodes[node.Right].Bounds.Union(Nodes[leaf].Bounds));
			// Equal costs take the lower flat node id, so allocator history cannot
			// choose a different sibling for equal spatial input.
			sibling = leftCost < rightCost	 ? node.Left
					  : rightCost < leftCost ? node.Right
											 : std::min(node.Left, node.Right);
		}

		const uint32_t oldParent = Nodes[sibling].Parent;
		const uint32_t parent = AllocateNode();
		Nodes[parent].Parent = oldParent;
		Nodes[parent].Left = sibling;
		Nodes[parent].Right = leaf;
		Nodes[parent].Bounds = Nodes[sibling].Bounds.Union(Nodes[leaf].Bounds);
		Nodes[sibling].Parent = parent;
		Nodes[leaf].Parent = parent;
		if (oldParent == INVALID_NODE) {
			Root = parent;
		} else if (Nodes[oldParent].Left == sibling) {
			Nodes[oldParent].Left = parent;
		} else {
			Nodes[oldParent].Right = parent;
		}
		Refit(parent);
	}

	void DynamicBvh::RemoveLeaf(uint32_t leaf) {
		if (leaf == Root) {
			Root = INVALID_NODE;
			return;
		}
		const uint32_t parent = Nodes[leaf].Parent;
		const uint32_t grandparent = Nodes[parent].Parent;
		const uint32_t sibling = Nodes[parent].Left == leaf ? Nodes[parent].Right : Nodes[parent].Left;
		if (grandparent == INVALID_NODE) {
			Root = sibling;
			Nodes[sibling].Parent = INVALID_NODE;
		} else {
			if (Nodes[grandparent].Left == parent) {
				Nodes[grandparent].Left = sibling;
			} else {
				Nodes[grandparent].Right = sibling;
			}
			Nodes[sibling].Parent = grandparent;
			Refit(grandparent);
		}
		FreeNode(parent);
		Nodes[leaf].Parent = INVALID_NODE;
	}

	uint32_t DynamicBvh::BuildBalanced(size_t first, size_t last, uint32_t parent) {
		const uint32_t node = AllocateNode();
		Nodes[node].Parent = parent;
		if (last - first == 1) {
			const uint32_t proxy = BuildOrder[first];
			Nodes[node].ProxyIndex = proxy;
			Nodes[node].Bounds = FatBounds(Proxies[proxy].Bounds);
			LeafNodes[proxy] = node;
			return node;
		}
		const size_t middle = first + (last - first) / 2;
		Nodes[node].Left = BuildBalanced(first, middle, node);
		Nodes[node].Right = BuildBalanced(middle, last, node);
		Nodes[node].Bounds = Nodes[Nodes[node].Left].Bounds.Union(Nodes[Nodes[node].Right].Bounds);
		return node;
	}

	void DynamicBvh::Clear() {
		Proxies.clear();
		Nodes.clear();
		LeafNodes.clear();
		BuildOrder.clear();
		BuildKeys.clear();
		FreeNodes.clear();
		CandidatePairs.clear();
		PairScratch.clear();
		EscapedProxyIndices.clear();
		EscapedProxyFlags.clear();
		PairCacheAvailable = true;
		Root = INVALID_NODE;
		EscapedLeaves = 0;
		RefittedLeaves = 0;
		ReinsertedLeaves = 0;
		CachedHeight = 0;
		CacheNodesVisited = 0;
		SyncGeneration++;
		PublishMetrics();
	}

	void DynamicBvh::Rebuild(std::span<const Proxy> proxies) {
		if (proxies.data() != Proxies.data()) {
			Proxies.assign(proxies.begin(), proxies.end());
		}
		Nodes.clear();
		LeafNodes.clear();
		BuildOrder.resize(Proxies.size());
		BuildKeys.resize(Proxies.size());
		FreeNodes.clear();
		Root = INVALID_NODE;
		LeafNodes.reserve(Proxies.size());
		LeafNodes.resize(Proxies.size());
		std::iota(BuildOrder.begin(), BuildOrder.end(), 0u);
		core::Vector3 minimum{
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max()
		};
		core::Vector3 maximum{
			-std::numeric_limits<float>::max(),
			-std::numeric_limits<float>::max(),
			-std::numeric_limits<float>::max()
		};
		for (const Proxy &proxy : Proxies) {
			if (!Finite(proxy.Bounds)) {
				continue;
			}
			minimum = core::Vector3{
				std::min(minimum.X, proxy.Bounds.Minimum.X),
				std::min(minimum.Y, proxy.Bounds.Minimum.Y),
				std::min(minimum.Z, proxy.Bounds.Minimum.Z)
			};
			maximum = core::Vector3{
				std::max(maximum.X, proxy.Bounds.Maximum.X),
				std::max(maximum.Y, proxy.Bounds.Maximum.Y),
				std::max(maximum.Z, proxy.Bounds.Maximum.Z)
			};
		}
		const core::Vector3 extent = maximum - minimum;
		const float scale = std::max({extent.X, extent.Y, extent.Z});
		for (size_t index = 0; index < Proxies.size(); index++) {
			if (!Finite(Proxies[index].Bounds)) {
				BuildKeys[index] = UINT32_MAX;
				continue;
			}
			const core::Vector3 centre = Proxies[index].Bounds.Centre();
			const auto quantize = [](float value, float low, float span) {
				if (!(span > 0.0f)) return 0u;
				return static_cast<uint32_t>(std::clamp((value - low) * 1023.0f / span, 0.0f, 1023.0f));
			};
			const uint32_t x = quantize(centre.X, minimum.X, scale);
			const uint32_t y = quantize(centre.Y, minimum.Y, scale);
			const uint32_t z = quantize(centre.Z, minimum.Z, scale);
			uint32_t key = 0;
			for (uint32_t bit = 0; bit < 10; bit++) {
				key |= ((x >> bit) & 1u) << (bit * 3);
				key |= ((y >> bit) & 1u) << (bit * 3 + 1);
				key |= ((z >> bit) & 1u) << (bit * 3 + 2);
			}
			BuildKeys[index] = key;
		}
		std::sort(BuildOrder.begin(), BuildOrder.end(), [this](uint32_t left, uint32_t right) {
			return BuildKeys[left] == BuildKeys[right] ? left < right : BuildKeys[left] < BuildKeys[right];
		});
		if (!Proxies.empty()) {
			Root = BuildBalanced(0, Proxies.size(), INVALID_NODE);
		}
		CachedHeight = Height(Root);
		BuildPairCache();
		EscapedLeaves = 0;
		RefittedLeaves = 0;
		ReinsertedLeaves = 0;
		CacheNodesVisited = 0;
		RebuildCount++;
		SyncGeneration++;
		PublishMetrics();
	}

	DynamicBvhPreflight DynamicBvh::Preflight(std::span<const Proxy> proxies) const {
		if (proxies.size() != Proxies.size()) {
			return DynamicBvhPreflight{};
		}
		for (size_t index = 0; index < proxies.size(); index++) {
			if (proxies[index].Id != Proxies[index].Id) {
				return DynamicBvhPreflight{};
			}
		}
		size_t escaped = 0;
		for (size_t index = 0; index < proxies.size(); index++) {
			if (!Contains(Nodes[LeafNodes[index]].Bounds, proxies[index].Bounds)) {
				escaped++;
			}
		}
		return DynamicBvhPreflight{true, escaped, proxies.data(), SyncGeneration};
	}

	bool DynamicBvh::Sync(std::span<const Proxy> proxies, const DynamicBvhPreflight &preflight) {
		if (!preflight.Compatible || preflight.Source != proxies.data() ||
			preflight.Generation != SyncGeneration || proxies.size() != Proxies.size()) {
			return false;
		}
		EscapedLeaves = preflight.EscapedLeaves;

		RefittedLeaves = 0;
		ReinsertedLeaves = 0;
		CacheNodesVisited = 0;
		EscapedProxyIndices.clear();
		EscapedProxyFlags.assign(Proxies.size(), 0);
		// A handful of escaped leaves can retain topology. Their fresh predicted
		// leaves replace, rather than accumulate on, old fat bounds so traversal
		// quality and the cached candidate set remain bounded over long motion.
		const bool refitSmallEscape = preflight.EscapedLeaves <= 4;
		for (size_t index = 0; index < proxies.size(); index++) {
			const core::AABB previous = Proxies[index].Bounds;
			Proxies[index] = proxies[index];
			const uint32_t leaf = LeafNodes[index];
			if (Contains(Nodes[leaf].Bounds, proxies[index].Bounds)) {
				continue;
			}
			if (refitSmallEscape) {
				Nodes[leaf].Bounds = PredictedFatBounds(previous, proxies[index].Bounds);
				Refit(Nodes[leaf].Parent);
				EscapedProxyIndices.push_back(static_cast<uint32_t>(index));
				EscapedProxyFlags[index] = 1;
				RefittedLeaves++;
				continue;
			}
			RemoveLeaf(leaf);
			Nodes[leaf].Bounds = PredictedFatBounds(previous, proxies[index].Bounds);
			InsertLeaf(leaf);
			EscapedProxyIndices.push_back(static_cast<uint32_t>(index));
			EscapedProxyFlags[index] = 1;
			ReinsertedLeaves++;
		}
		if (!EscapedProxyIndices.empty()) {
			UpdatePairCache();
		}
		if (ReinsertedLeaves != 0) {
			CachedHeight = Height(Root);
		}
		const size_t heightLimit =
			Proxies.empty() ? 0 : 2 * static_cast<size_t>(std::ceil(std::log2(Proxies.size()))) + 1;
		if (CachedHeight > heightLimit) {
			QualityRebuildCount++;
			Rebuild(Proxies);
			return true;
		}
		SyncGeneration++;
		if (EscapedLeaves != 0) {
			PublishMetrics();
		}
		return true;
	}

	size_t DynamicBvh::Height(uint32_t node) const {
		if (node == INVALID_NODE) {
			return 0;
		}
		const Node &current = Nodes[node];
		if (current.ProxyIndex != INVALID_NODE) {
			return 1;
		}
		return 1 + std::max(Height(current.Left), Height(current.Right));
	}

	DynamicBvhStats DynamicBvh::Stats() const {
		const size_t liveNodes = Proxies.empty() ? 0 : Proxies.size() * 2 - 1;
		return DynamicBvhStats{
			Proxies.size(),
			EscapedLeaves,
			RefittedLeaves,
			ReinsertedLeaves,
			RebuildCount,
			QualityRebuildCount,
			CandidatePairs.size(),
			CacheNodesVisited,
			PairCacheAvailable,
			Proxies.size() * sizeof(Proxy) + liveNodes * sizeof(Node) + LeafNodes.size() * sizeof(uint32_t) +
				BuildOrder.size() * sizeof(uint32_t) + BuildKeys.size() * sizeof(uint32_t) +
				CandidatePairs.size() * sizeof(uint64_t),
			Proxies.capacity() * sizeof(Proxy) + Nodes.capacity() * sizeof(Node) +
				LeafNodes.capacity() * sizeof(uint32_t) + BuildOrder.capacity() * sizeof(uint32_t) +
				BuildKeys.capacity() * sizeof(uint32_t) + FreeNodes.capacity() * sizeof(uint32_t) +
				CandidatePairs.capacity() * sizeof(uint64_t) + PairScratch.capacity() * sizeof(uint64_t) +
				EscapedProxyIndices.capacity() * sizeof(uint32_t) +
				EscapedProxyFlags.capacity() * sizeof(uint8_t),
			CachedHeight,
		};
	}

	void DynamicBvh::PublishMetrics() const {
		const DynamicBvhStats stats = Stats();
		core::Metrics::SetGauge("spatial.bvh.proxies", static_cast<double>(stats.ProxyCount));
		core::Metrics::SetGauge("spatial.bvh.escaped", static_cast<double>(stats.EscapedLeaves));
		core::Metrics::SetGauge("spatial.bvh.refitted", static_cast<double>(stats.RefittedLeaves));
		core::Metrics::SetGauge("spatial.bvh.reinserted", static_cast<double>(stats.ReinsertedLeaves));
		core::Metrics::SetGauge("spatial.bvh.quality_rebuilds", static_cast<double>(stats.QualityRebuilds));
		core::Metrics::SetGauge("spatial.bvh.cached_pairs", static_cast<double>(stats.CachedPairs));
		core::Metrics::SetGauge(
			"spatial.bvh.cache_nodes_visited", static_cast<double>(stats.CacheNodesVisited)
		);
		core::Metrics::SetGauge("spatial.bvh.pair_cache_available", stats.PairCacheAvailable ? 1.0 : 0.0);
		core::Metrics::SetGauge("spatial.bvh.live_bytes", static_cast<double>(stats.LiveBytes));
		core::Metrics::SetGauge("spatial.bvh.retained_bytes", static_cast<double>(stats.RetainedBytes));
	}

	void DynamicBvh::BuildPairCache() {
		CandidatePairs.clear();
		PairCacheAvailable = true;
		if (Root == INVALID_NODE) {
			return;
		}
		auto cachePair = [this](const Proxy &first, const Proxy &second) {
			if (CandidatePairs.size() == MAXIMUM_CACHED_PAIRS) {
				PairCacheAvailable = false;
				CandidatePairs.clear();
				return false;
			}
			const uint32_t left = static_cast<uint32_t>(&first - Proxies.data());
			const uint32_t right = static_cast<uint32_t>(&second - Proxies.data());
			CandidatePairs.push_back(
				(static_cast<uint64_t>(std::min(left, right)) << 32) | std::max(left, right)
			);
			return true;
		};
		ForEachPairNodes(Root, Root, cachePair);
		if (PairCacheAvailable) {
			std::sort(CandidatePairs.begin(), CandidatePairs.end());
			CandidatePairs.erase(
				std::unique(CandidatePairs.begin(), CandidatePairs.end()), CandidatePairs.end()
			);
		}
	}

	void DynamicBvh::AppendPairsForLeaf(uint32_t proxyIndex) {
		const core::AABB &volume = Nodes[LeafNodes[proxyIndex]].Bounds;
		uint32_t node = Root;
		uint32_t previous = INVALID_NODE;
		while (node != INVALID_NODE) {
			CacheNodesVisited++;
			const Node &current = Nodes[node];
			uint32_t next = current.Parent;
			if (previous == current.Parent && current.Bounds.Overlaps(volume)) {
				if (current.ProxyIndex == INVALID_NODE) {
					next = current.Left;
				} else if (current.ProxyIndex != proxyIndex) {
					if (CandidatePairs.size() + PairScratch.size() == MAXIMUM_CACHED_PAIRS) {
						PairCacheAvailable = false;
						return;
					}
					const uint32_t other = current.ProxyIndex;
					PairScratch.push_back(
						(static_cast<uint64_t>(std::min(proxyIndex, other)) << 32) |
						std::max(proxyIndex, other)
					);
				}
			} else if (previous == current.Left) {
				next = current.Right;
			}
			previous = node;
			node = next;
		}
	}

	void DynamicBvh::UpdatePairCache() {
		if (!PairCacheAvailable) {
			return;
		}
		CandidatePairs.erase(
			std::remove_if(
				CandidatePairs.begin(),
				CandidatePairs.end(),
				[this](uint64_t pair) {
					return EscapedProxyFlags[static_cast<uint32_t>(pair >> 32)] != 0 ||
						   EscapedProxyFlags[static_cast<uint32_t>(pair)] != 0;
				}
			),
			CandidatePairs.end()
		);
		PairScratch.clear();
		for (uint32_t proxyIndex : EscapedProxyIndices) {
			AppendPairsForLeaf(proxyIndex);
			if (!PairCacheAvailable) {
				CandidatePairs.clear();
				PairScratch.clear();
				return;
			}
		}
		std::sort(PairScratch.begin(), PairScratch.end());
		const size_t retained = CandidatePairs.size();
		CandidatePairs.insert(CandidatePairs.end(), PairScratch.begin(), PairScratch.end());
		std::inplace_merge(CandidatePairs.begin(), CandidatePairs.begin() + retained, CandidatePairs.end());
		CandidatePairs.erase(std::unique(CandidatePairs.begin(), CandidatePairs.end()), CandidatePairs.end());
		PairScratch.clear();
		EscapedProxyIndices.clear();
		EscapedProxyFlags.clear();
	}

	std::optional<core::RayHit>
	Raycast(const DynamicBvh &tree, const core::Ray &ray, float maxDistance, LayerMask mask) {
		if (!DynamicCanTravel(ray, maxDistance)) {
			return std::nullopt;
		}
		const RayReciprocal reciprocal{ray.Direction};
		std::optional<core::RayHit> nearest;
		DynamicBvhInternals::ForEachCandidate(
			tree, SegmentBounds(ray, maxDistance), mask, [&](const Proxy &proxy) {
				const BoxHit hit = IntersectRayBox(ray, reciprocal, proxy.Bounds, maxDistance);
				if (hit.Touched && (!nearest || hit.Distance < nearest->Distance ||
									(hit.Distance == nearest->Distance && proxy.Id < nearest->Id))) {
					nearest = core::RayHit{proxy.Id, hit.Distance, ray.PointAt(hit.Distance), hit.Normal};
				}
				return true;
			}
		);
		return nearest;
	}

	QueryResult RaycastAll(
		const DynamicBvh &tree,
		const core::Ray &ray,
		float maxDistance,
		LayerMask mask,
		std::span<core::RayHit> hits
	) {
		QueryResult result;
		if (!DynamicCanTravel(ray, maxDistance)) {
			return result;
		}
		const RayReciprocal reciprocal{ray.Direction};
		DynamicBvhInternals::ForEachCandidate(
			tree, SegmentBounds(ray, maxDistance), mask, [&](const Proxy &proxy) {
				const BoxHit hit = IntersectRayBox(ray, reciprocal, proxy.Bounds, maxDistance);
				if (hit.Touched) {
					DynamicInsertNearest(
						hits,
						core::RayHit{proxy.Id, hit.Distance, ray.PointAt(hit.Distance), hit.Normal},
						result
					);
				}
				return true;
			}
		);
		return result;
	}

	QueryResult
	OverlapBox(const DynamicBvh &tree, const core::AABB &box, LayerMask mask, std::span<uint64_t> found) {
		QueryResult result;
		DynamicBvhInternals::ForEachCandidate(tree, box, mask, [&](const Proxy &proxy) {
			return InsertId(found, proxy.Id, result);
		});
		return result;
	}

	QueryResult OverlapSphere(
		const DynamicBvh &tree,
		const core::Vector3 &centre,
		float radius,
		LayerMask mask,
		std::span<uint64_t> found
	) {
		QueryResult result;
		if (!(radius >= 0.0f)) {
			return result;
		}
		const core::AABB volume = core::AABB::FromCentre(centre, core::Vector3{radius, radius, radius});
		const float radiusSquared = radius * radius;
		DynamicBvhInternals::ForEachCandidate(tree, volume, mask, [&](const Proxy &proxy) {
			const core::Vector3 offset = proxy.Bounds.ClosestPoint(centre) - centre;
			return offset.MagnitudeSquared() > radiusSquared || InsertId(found, proxy.Id, result);
		});
		return result;
	}

	QueryResult ShapeCast(
		const DynamicBvh &tree,
		const core::AABB &box,
		const core::Vector3 &motion,
		LayerMask mask,
		std::span<uint64_t> found
	) {
		if (motion == core::Vector3::Zero) {
			return OverlapBox(tree, box, mask, found);
		}
		QueryResult result;
		const float distance = motion.Magnitude();
		if (!(distance > 0.0f)) {
			return result;
		}
		const core::Vector3 halfExtent = box.Size() * 0.5f;
		const core::Ray ray{box.Centre(), motion / distance};
		const RayReciprocal reciprocal{ray.Direction};
		const core::AABB swept = box.Union(core::AABB{box.Minimum + motion, box.Maximum + motion});
		DynamicBvhInternals::ForEachCandidate(tree, swept, mask, [&](const Proxy &proxy) {
			const core::AABB expanded =
				core::AABB::FromCentre(proxy.Bounds.Centre(), proxy.Bounds.Size() * 0.5f + halfExtent);
			return !IntersectRayBox(ray, reciprocal, expanded, distance).Touched ||
				   InsertId(found, proxy.Id, result);
		});
		return result;
	}
}
