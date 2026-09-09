#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <span>
#include <vector>

namespace engine::render {

	struct FrameViewIdentity {
		uint64_t World = 0;
		const void *Pipeline = nullptr;
	};

	struct FrameViewGroup {
		FrameViewIdentity Identity;
		std::vector<size_t> Views;
	};

	// Resolved pipeline identity keeps fallback aliases in one contiguous world.
	inline std::vector<FrameViewGroup>
	GroupFrameViews(std::span<const FrameViewIdentity> views, bool present) {
		std::vector<FrameViewGroup> groups;
		groups.reserve(views.size());
		for (size_t index = 0; index < views.size(); index++) {
			const FrameViewIdentity &view = views[index];
			auto found = std::find_if(groups.begin(), groups.end(), [&](const FrameViewGroup &group) {
				return group.Identity.World == view.World && group.Identity.Pipeline == view.Pipeline;
			});
			if (found == groups.end()) {
				groups.push_back({view, {index}});
			} else {
				found->Views.push_back(index);
			}
		}
		if (!present || views.empty()) {
			return groups;
		}
		const auto finalGroup = std::find_if(groups.begin(), groups.end(), [&](const FrameViewGroup &group) {
			return group.Views.back() == views.size() - 1;
		});
		std::rotate(finalGroup, std::next(finalGroup), groups.end());
		return groups;
	}

	// Tracks accepted setup within one batch. Cached views leave it unchanged.
	class FramePreparation {
	  public:
		bool NeedsFrame(const void *pipeline) const {
			return std::none_of(Prepared.begin(), Prepared.end(), [pipeline](const Entry &entry) {
				return entry.Pipeline == pipeline;
			});
		}

		bool NeedsWorld(const void *pipeline, uint64_t world) const {
			return std::none_of(Prepared.begin(), Prepared.end(), [=](const Entry &entry) {
				return entry.Pipeline == pipeline && entry.World == world && entry.WorldReady;
			});
		}

		void Complete(const void *pipeline, uint64_t world, bool executed) {
			if (!executed) {
				return;
			}
			auto found = std::find_if(Prepared.begin(), Prepared.end(), [=](const Entry &entry) {
				return entry.Pipeline == pipeline && entry.World == world;
			});
			if (found == Prepared.end()) {
				Prepared.push_back({pipeline, world, true});
			} else {
				found->WorldReady = true;
			}
		}

		// A view-specific shadow map consumes shared storage without invalidating frame effects.
		void InvalidateWorld(const void *pipeline, uint64_t world) {
			for (Entry &entry : Prepared)
				if (entry.Pipeline == pipeline && entry.World == world) entry.WorldReady = false;
		}

		void Clear() {
			Prepared.clear();
		}

	  private:
		struct Entry {
			const void *Pipeline = nullptr;
			uint64_t World = 0;
			bool WorldReady = false;
		};
		std::vector<Entry> Prepared;
	};
}
