#pragma once

#include <algorithm>
#include <cstddef>
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
	inline std::span<const FrameViewGroup> GroupFrameViews(
		std::span<const FrameViewIdentity> views, bool present, std::vector<FrameViewGroup> &groups
	) {
		for (FrameViewGroup &group : groups) {
			group.Identity = {};
			group.Views.clear();
		}
		groups.reserve(views.size());
		size_t active = 0;
		for (size_t index = 0; index < views.size(); index++) {
			const FrameViewIdentity &view = views[index];
			const auto end = groups.begin() + static_cast<std::ptrdiff_t>(active);
			auto found = std::find_if(groups.begin(), end, [&](const FrameViewGroup &group) {
				return group.Identity.World == view.World && group.Identity.Pipeline == view.Pipeline;
			});
			if (found == end) {
				if (active == groups.size()) groups.emplace_back();
				FrameViewGroup &group = groups[active++];
				group.Identity = view;
				group.Views.push_back(index);
			} else {
				found->Views.push_back(index);
			}
		}
		const std::span<const FrameViewGroup> result(groups.data(), active);
		if (!present || views.empty()) {
			return result;
		}
		const auto end = groups.begin() + static_cast<std::ptrdiff_t>(active);
		const auto finalGroup = std::find_if(groups.begin(), end, [&](const FrameViewGroup &group) {
			return group.Views.back() == views.size() - 1;
		});
		std::rotate(finalGroup, std::next(finalGroup), end);
		return result;
	}

	inline std::vector<FrameViewGroup>
	GroupFrameViews(std::span<const FrameViewIdentity> views, bool present) {
		std::vector<FrameViewGroup> groups;
		const size_t active = GroupFrameViews(views, present, groups).size();
		groups.resize(active);
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
