#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <cdn/Grouper.hpp>
#include <map>

namespace cdn {

	namespace {
		using engine::assets::ContentHash;

		// An affinity's worth of assets, treated as one indivisible lump.
		//
		// Rule 1 outranks rule 2, so this is the thing that gets packed rather
		// than the individual asset. Everything below works in these.
		struct Cluster {
			std::vector<ContentHash> Assets;
			uint64_t Bytes = 0;
			uint32_t Priority = 0;
		};

		// Groups candidates into clusters by affinity.
		//
		// Affinity zero is the exception: it means "belongs with nothing in
		// particular", so each such asset is its own cluster. Treating zero as
		// an affinity like any other would bind every unrelated asset in the
		// game into one lump, which is the opposite of what the value means.
		std::vector<Cluster> BuildClusters(std::span<const GroupCandidate> candidates) {
			std::map<uint32_t, Cluster> byAffinity;
			std::vector<Cluster> loose;

			for (const GroupCandidate &candidate : candidates) {
				if (candidate.Affinity == 0) {
					Cluster single;
					single.Assets.push_back(candidate.Root);
					single.Bytes = candidate.Bytes;
					single.Priority = candidate.Priority;
					loose.push_back(std::move(single));
					continue;
				}

				Cluster &cluster = byAffinity[candidate.Affinity];
				if (cluster.Assets.empty()) {
					cluster.Priority = candidate.Priority;
				} else {
					// A cluster is wanted as soon as its most urgent member is.
					cluster.Priority = std::min(cluster.Priority, candidate.Priority);
				}
				cluster.Assets.push_back(candidate.Root);
				cluster.Bytes += candidate.Bytes;
			}

			std::vector<Cluster> clusters;
			clusters.reserve(byAffinity.size() + loose.size());
			// std::map, so affinities come out in a defined order regardless of
			// the order the candidates arrived in. Determinism is a requirement
			// here rather than a nicety: two origins that group the same content
			// differently prepare and cache different bundles for it, and
			// nothing reports that they have stopped sharing.
			for (auto &entry : byAffinity) {
				clusters.push_back(std::move(entry.second));
			}
			for (Cluster &cluster : loose) {
				clusters.push_back(std::move(cluster));
			}
			return clusters;
		}
	}

	bool GroupPolicy::IsValid() const {
		return TargetBytes > 0 && TargetBytes <= MaximumBytes;
	}

	Grouper::Grouper(GroupPolicy policy) : Envelope(policy.IsValid() ? policy : GroupPolicy{}) {}

	Assembly Grouper::Assemble(std::span<const GroupCandidate> candidates) const {
		ENGINE_PROFILE_CAT("Grouper::Assemble", engine::core::ProfileCategory::Assets);

		Assembly assembly;
		if (candidates.empty()) {
			return assembly;
		}

		std::vector<Cluster> clusters = BuildClusters(candidates);

		// Priority first, then heaviest first within a priority.
		//
		// The size order is what produces rule 2's mix without a rule of its
		// own: packing large clusters first leaves the small ones to fill the
		// space each large one did not use, so a group ends up with a few big
		// members and many small ones. Packing smallest-first would fill whole
		// groups with tiny assets and leave the large ones alone in groups of
		// their own — every group then pathological in one direction or the
		// other.
		//
		// The root is the last tiebreak so the order is total: two clusters of
		// equal priority and equal weight must not be ordered by whichever the
		// caller happened to list first.
		std::sort(clusters.begin(), clusters.end(), [](const Cluster &left, const Cluster &right) {
			if (left.Priority != right.Priority) {
				return left.Priority < right.Priority;
			}
			if (left.Bytes != right.Bytes) {
				return left.Bytes > right.Bytes;
			}
			return left.Assets.front() < right.Assets.front();
		});

		Group current;
		// No oversized check in here, and it is not an omission. A packed group
		// is flushed before a cluster would take it past the target, so it can
		// only ever weigh at most one cluster more than empty — and a cluster
		// heavier than the ceiling is diverted below and never reaches this. The
		// one path that can produce an oversized group is the one that counts it.
		const auto flush = [&assembly, &current]() {
			if (current.Assets.empty()) {
				return;
			}
			std::sort(current.Assets.begin(), current.Assets.end());
			assembly.Groups.push_back(std::move(current));
			current = Group{};
		};

		for (Cluster &cluster : clusters) {
			// A cluster heavier than the ceiling on its own becomes one
			// oversized group. Rule 1 outranks the bound: splitting it would
			// produce two groups neither of which makes anything appear, which
			// is the one outcome this whole class exists to avoid. It is
			// counted rather than hidden.
			if (cluster.Bytes > Envelope.MaximumBytes) {
				flush();
				Group alone;
				alone.Assets = std::move(cluster.Assets);
				alone.TotalBytes = cluster.Bytes;
				alone.Priority = cluster.Priority;
				std::sort(alone.Assets.begin(), alone.Assets.end());
				++assembly.Oversized;
				assembly.Groups.push_back(std::move(alone));
				continue;
			}

			if (!current.Assets.empty() && current.TotalBytes + cluster.Bytes > Envelope.TargetBytes) {
				flush();
			}

			if (current.Assets.empty()) {
				current.Priority = cluster.Priority;
			}
			current.Assets.insert(current.Assets.end(), cluster.Assets.begin(), cluster.Assets.end());
			current.TotalBytes += cluster.Bytes;
		}

		flush();

		engine::core::Metrics::Count("cdn.groups.assembled", static_cast<double>(assembly.Groups.size()));
		if (assembly.Oversized > 0) {
			engine::core::Metrics::Count("cdn.groups.oversized", static_cast<double>(assembly.Oversized));
		}
		return assembly;
	}
}
