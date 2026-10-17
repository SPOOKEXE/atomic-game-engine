#include <engine/weather/Residency.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <tuple>

namespace engine::weather {
	namespace {
		bool KeyBefore(const BrickKey &first, const BrickKey &second) {
			return std::tie(first.Lod, first.Origin.X, first.Origin.Y, first.Origin.Z) <
				   std::tie(second.Lod, second.Origin.X, second.Origin.Y, second.Origin.Z);
		}

		bool FiniteNonnegative(float amount) {
			return std::isfinite(amount) && amount >= 0.0f;
		}

		bool ValidViews(std::span<const ResidencyView> views) {
			if (views.size() > MAXIMUM_RESIDENCY_VIEWS) return false;
			for (size_t first = 0; first < views.size(); ++first) {
				const auto &position = views[first].CameraPositionMetres;
				if (!std::isfinite(position.X) || !std::isfinite(position.Y) || !std::isfinite(position.Z))
					return false;
				for (size_t second = first + 1; second < views.size(); ++second)
					if (views[first].Key == views[second].Key) return false;
			}
			return true;
		}

		void ClearPending(ResidencyState &state) {
			state.PendingGeneration = 0;
			state.PendingTick = 0;
			state.PendingRevision = 0;
			state.GenerationSubmission = 0;
		}
	}

	bool ValidBrickKey(const BrickKey &key) {
		// 8 * 2^27 cells still fits an int32 lattice extent; larger origins cannot form a brick.
		if (key.Lod > 27) return false;
		const int64_t side = static_cast<int64_t>(BRICK_SIDE) << key.Lod;
		const std::array<int64_t, 3> origin{key.Origin.X, key.Origin.Y, key.Origin.Z};
		for (int64_t axis : origin) {
			if (axis % side != 0 || axis + side - 1 > std::numeric_limits<int32_t>::max()) return false;
		}
		return true;
	}

	std::optional<ResidencySelection>
	SelectResidency(std::span<const BrickDemand> demands, uint32_t pageBudget) {
		if (demands.size() > MAXIMUM_RESIDENCY_DEMANDS || pageBudget > MAXIMUM_RESIDENCY_PAGES)
			return std::nullopt;
		for (const auto &demand : demands)
			if (!ValidBrickKey(demand.Key) || !FiniteNonnegative(demand.ProjectedPixels) ||
				!FiniteNonnegative(demand.MaximumOpticalError))
				return std::nullopt;
		std::vector<BrickDemand> combined(demands.begin(), demands.end());
		std::sort(combined.begin(), combined.end(), [](const auto &first, const auto &second) {
			return KeyBefore(first.Key, second.Key);
		});
		size_t uniqueCount = 0;
		for (const auto &demand : combined) {
			if (uniqueCount != 0 && combined[uniqueCount - 1].Key == demand.Key) {
				auto &retained = combined[uniqueCount - 1];
				retained.ProjectedPixels = std::max(retained.ProjectedPixels, demand.ProjectedPixels);
				retained.MaximumOpticalError =
					std::max(retained.MaximumOpticalError, demand.MaximumOpticalError);
				retained.RequiredCoarse = retained.RequiredCoarse || demand.RequiredCoarse;
			} else {
				combined[uniqueCount++] = demand;
			}
		}
		combined.resize(uniqueCount);
		std::sort(combined.begin(), combined.end(), [](const auto &first, const auto &second) {
			if (first.RequiredCoarse != second.RequiredCoarse) return first.RequiredCoarse;
			if (first.MaximumOpticalError != second.MaximumOpticalError)
				return first.MaximumOpticalError > second.MaximumOpticalError;
			if (first.ProjectedPixels != second.ProjectedPixels)
				return first.ProjectedPixels > second.ProjectedPixels;
			return KeyBefore(first.Key, second.Key);
		});
		ResidencySelection selection;
		selection.Pages.reserve(std::min(static_cast<size_t>(pageBudget), combined.size()));
		for (const auto &demand : combined) {
			if (selection.Pages.size() < pageBudget) {
				selection.Pages.push_back(demand.Key);
			} else if (demand.RequiredCoarse) {
				++selection.CoarseFallbackRegions;
				selection.RequiresProceduralFallback = true;
			} else {
				++selection.RefusedRefinements;
			}
		}
		std::sort(selection.Pages.begin(), selection.Pages.end(), KeyBefore);
		return selection;
	}

	std::optional<uint32_t> ChooseLod(
		float finestProjectedPixels,
		float finestOpticalError,
		uint32_t previousLod,
		uint64_t residenceTicks,
		const LodPolicy &policy
	) {
		if (!FiniteNonnegative(finestProjectedPixels) || !FiniteNonnegative(finestOpticalError) ||
			policy.MaximumLod > 27 || previousLod > policy.MaximumLod ||
			!FiniteNonnegative(policy.MaximumOpticalError) || !FiniteNonnegative(policy.CoarsenBelowPixels) ||
			!std::isfinite(policy.RefineAbovePixels) || policy.RefineAbovePixels <= policy.CoarsenBelowPixels)
			return std::nullopt;
		uint32_t lod = previousLod;
		const auto projected = [&](uint32_t level) {
			return std::ldexp(finestProjectedPixels, -static_cast<int>(level));
		};
		const auto error = [&](uint32_t level) {
			return std::ldexp(finestOpticalError, static_cast<int>(level));
		};
		// Optical violations override residence time; keeping an incorrect coarse field is unsafe.
		while (lod > 0 && error(lod) > policy.MaximumOpticalError)
			--lod;
		if (residenceTicks < policy.MinimumResidenceTicks) return lod;
		while (lod > 0 && projected(lod) > policy.RefineAbovePixels)
			--lod;
		while (lod < policy.MaximumLod && projected(lod) < policy.CoarsenBelowPixels &&
			   error(lod + 1) <= policy.MaximumOpticalError)
			++lod;
		return lod;
	}

	std::optional<float> MotionMargin(const MotionEnvelope &envelope, float lifetimeSeconds) {
		if (!FiniteNonnegative(lifetimeSeconds) || !FiniteNonnegative(envelope.MaximumSpeedMetresPerSecond) ||
			!FiniteNonnegative(envelope.MaximumAccelerationMetresPerSecondSquared) ||
			!FiniteNonnegative(envelope.CameraSpeedMetresPerSecond) ||
			!FiniteNonnegative(envelope.GrowthMetresPerSecond) ||
			!FiniteNonnegative(envelope.ReconstructionMarginMetres))
			return std::nullopt;
		const double lifetime = lifetimeSeconds;
		const double margin = (static_cast<double>(envelope.MaximumSpeedMetresPerSecond) +
							   envelope.CameraSpeedMetresPerSecond + envelope.GrowthMetresPerSecond) *
								  lifetime +
							  0.5 * envelope.MaximumAccelerationMetresPerSecondSquared * lifetime * lifetime +
							  envelope.ReconstructionMarginMetres;
		if (margin > std::numeric_limits<float>::max()) return std::nullopt;
		const float rounded = static_cast<float>(margin);
		return static_cast<double>(rounded) < margin
				   ? std::nextafter(rounded, std::numeric_limits<float>::infinity())
				   : rounded;
	}

	bool MaintenanceRequired(
		const MaintenanceStamp &stamp,
		std::span<const ResidencyView> views,
		uint64_t tick,
		uint32_t weatherRevision,
		uint64_t timelineRevision,
		const MaintenancePolicy &policy
	) {
		if (!stamp.Valid || !ValidViews(views) || !ValidViews(stamp.Views) ||
			views.size() != stamp.Views.size() || tick < stamp.Tick ||
			weatherRevision != stamp.WeatherRevision || timelineRevision != stamp.TimelineRevision ||
			policy.TickRate == 0 || !std::isfinite(policy.IntervalSeconds) ||
			policy.IntervalSeconds <= 0.0f || !FiniteNonnegative(policy.MaximumCameraTravelMetres))
			return true;
		const double elapsedSeconds = static_cast<double>(tick - stamp.Tick) / policy.TickRate;
		if (elapsedSeconds >= policy.IntervalSeconds) return true;
		for (const auto &view : views) {
			const auto previous =
				std::find_if(stamp.Views.begin(), stamp.Views.end(), [&](const auto &candidate) {
					return candidate.Key == view.Key;
				});
			if (previous == stamp.Views.end() || previous->PortalRevision != view.PortalRevision) return true;
			if ((view.CameraPositionMetres - previous->CameraPositionMetres).Magnitude() >
				policy.MaximumCameraTravelMetres)
				return true;
		}
		return false;
	}

	std::optional<MaintenanceStamp> RecordMaintenance(
		std::span<const ResidencyView> views,
		uint64_t tick,
		uint32_t weatherRevision,
		uint64_t timelineRevision
	) {
		if (!ValidViews(views)) return std::nullopt;
		MaintenanceStamp stamp;
		stamp.Tick = tick;
		stamp.WeatherRevision = weatherRevision;
		stamp.TimelineRevision = timelineRevision;
		stamp.Views.assign(views.begin(), views.end());
		stamp.Valid = true;
		return stamp;
	}

	std::optional<PageGeneration> ReserveGeneration(
		ResidencyState &state,
		std::span<const BrickKey> pages,
		uint64_t tick,
		uint32_t revision,
		uint64_t completedSubmission
	) {
		if (state.PendingGeneration != 0 || state.NextGeneration == 0 ||
			state.NextGeneration == std::numeric_limits<uint64_t>::max() ||
			pages.size() > MAXIMUM_RESIDENCY_PAGES || state.Slots.size() > MAXIMUM_RESIDENCY_PAGES * 2)
			return std::nullopt;
		std::vector<BrickKey> ordered(pages.begin(), pages.end());
		std::sort(ordered.begin(), ordered.end(), KeyBefore);
		for (size_t index = 0; index < ordered.size(); ++index)
			if (!ValidBrickKey(ordered[index]) || (index != 0 && ordered[index] == ordered[index - 1]))
				return std::nullopt;
		PageGeneration generation;
		generation.Generation = state.NextGeneration;
		generation.Slots.reserve(ordered.size());
		for (size_t index = 0; index < state.Slots.size() && generation.Slots.size() < ordered.size();
			 ++index) {
			const auto &slot = state.Slots[index];
			if (slot.Lifetime == PageLifetime::Vacant ||
				(slot.Lifetime == PageLifetime::Retired &&
				 slot.LastReferenceSubmission <= completedSubmission))
				generation.Slots.push_back(static_cast<uint32_t>(index));
		}
		if (generation.Slots.size() != ordered.size()) return std::nullopt;
		for (size_t index = 0; index < ordered.size(); ++index)
			state.Slots[generation.Slots[index]] = {
				ordered[index], generation.Generation, 0, PageLifetime::Generating
			};
		state.PendingGeneration = generation.Generation;
		state.PendingTick = tick;
		state.PendingRevision = revision;
		state.GenerationSubmission = 0;
		++state.NextGeneration;
		return generation;
	}

	bool MarkGenerationSubmitted(ResidencyState &state, uint64_t generation, uint64_t submission) {
		if (generation == 0 || generation != state.PendingGeneration || submission == 0 ||
			state.GenerationSubmission != 0)
			return false;
		state.GenerationSubmission = submission;
		return true;
	}

	bool PublishGeneration(ResidencyState &state, uint64_t generation, uint64_t completedSubmission) {
		if (generation == 0 || generation != state.PendingGeneration || state.GenerationSubmission == 0 ||
			state.GenerationSubmission > completedSubmission)
			return false;
		for (auto &slot : state.Slots) {
			if (slot.Lifetime == PageLifetime::Published)
				slot.Lifetime = PageLifetime::Retired;
			else if (slot.Lifetime == PageLifetime::Generating && slot.Generation == generation) {
				slot.Lifetime = PageLifetime::Published;
				slot.LastReferenceSubmission = state.GenerationSubmission;
			}
		}
		state.PublishedGeneration = generation;
		state.PublishedTick = state.PendingTick;
		state.PublishedRevision = state.PendingRevision;
		ClearPending(state);
		return true;
	}

	bool ReferencePublished(ResidencyState &state, uint64_t submission) {
		if (state.PublishedGeneration == 0 || submission == 0) return false;
		for (auto &slot : state.Slots)
			if (slot.Lifetime == PageLifetime::Published)
				slot.LastReferenceSubmission = std::max(slot.LastReferenceSubmission, submission);
		return true;
	}

	void RetireCompleted(ResidencyState &state, uint64_t completedSubmission) {
		for (auto &slot : state.Slots)
			if (slot.Lifetime == PageLifetime::Retired && slot.LastReferenceSubmission <= completedSubmission)
				slot = {};
	}

	bool CancelGeneration(ResidencyState &state, uint64_t generation) {
		if (generation == 0 || generation != state.PendingGeneration) return false;
		for (auto &slot : state.Slots) {
			if (slot.Lifetime != PageLifetime::Generating || slot.Generation != generation) continue;
			if (state.GenerationSubmission == 0)
				slot = {};
			else {
				slot.Lifetime = PageLifetime::Retired;
				slot.LastReferenceSubmission = state.GenerationSubmission;
			}
		}
		ClearPending(state);
		return true;
	}

	ResidencyMemory MemoryOf(const ResidencyState &state) {
		ResidencyMemory memory;
		memory.PayloadBytes = static_cast<uint64_t>(state.Slots.size()) * BRICK_PAYLOAD_BYTES;
		memory.MetadataBytes =
			sizeof(ResidencyState) + static_cast<uint64_t>(state.Slots.capacity()) * sizeof(ResidencyPage);
		for (const auto &slot : state.Slots) {
			if (slot.Lifetime == PageLifetime::Published)
				memory.PublishedBytes += BRICK_PAYLOAD_BYTES;
			else if (slot.Lifetime == PageLifetime::Generating)
				memory.GeneratingBytes += BRICK_PAYLOAD_BYTES;
			else if (slot.Lifetime == PageLifetime::Retired)
				memory.RetiredBytes += BRICK_PAYLOAD_BYTES;
		}
		return memory;
	}
}
