#pragma once

#include <engine/core/Name.hpp>
#include <engine/scene/DrawInstance.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::render {

	inline constexpr size_t MAX_VISIBILITY_OBSERVATIONS = 4096;

	enum class VisibilityState : uint8_t {
		Unavailable,
		SubmittedOpaqueOrMasked,
		SubmittedBlended,
		FrustumCulled,
		DepthOccluded,
		GpuIndirectCandidate
	};
	enum class VisibilityCause : uint8_t {
		Unavailable,
		OpaqueOrMaskedDraw,
		BlendedDraw,
		Frustum,
		ExactDepth,
		GpuIndirectEligibility
	};

	const char *Describe(VisibilityState state);
	const char *Describe(VisibilityCause cause);

	struct VisibilityObservation {
		core::Name World;
		uint64_t Entity = 0;
		VisibilityState State = VisibilityState::Unavailable;
		VisibilityCause Cause = VisibilityCause::Unavailable;
	};

	// An owned copy of one completed submission. Invalid snapshots have no rows.
	struct VisibilitySnapshot {
		uint64_t Frame = 0;
		size_t ViewSlot = 0;
		core::Name World;
		bool Valid = false;
		size_t Dropped = 0;
		bool DroppedExact = true;
		std::vector<VisibilityObservation> Observations;
	};

	// Fixed rows and an open-addressed index keep duplicate variants attached to
	// their source entity without a quadratic search.
	class VisibilityObservations {
	  public:
		void Begin(uint64_t frame, size_t viewSlot, core::Name world);
		void Invalidate();
		void Observe(const scene::DrawInstance &instance);
		void Submitted(const scene::DrawInstance &instance);
		void GpuIndirectCandidate(const scene::DrawInstance &instance);
		void FrustumCulled(const scene::DrawInstance &instance);
		VisibilitySnapshot Snapshot() const;

	  private:
		VisibilityObservation *Find(const scene::DrawInstance &instance, bool create);
		static size_t Hash(core::Name world, uint64_t entity);

		static constexpr size_t INDEX_CAPACITY = MAX_VISIBILITY_OBSERVATIONS * 2;
		core::Name ViewWorld;
		uint64_t Frame = 0;
		size_t ViewSlot = 0;
		bool Active = false;
		std::array<VisibilityObservation, MAX_VISIBILITY_OBSERVATIONS> Rows{};
		std::array<int32_t, INDEX_CAPACITY> Index{};
		std::array<VisibilityObservation, MAX_VISIBILITY_OBSERVATIONS> Overflow{};
		size_t Count = 0;
		size_t OverflowCount = 0;
		size_t Dropped = 0;
		bool DroppedExact = true;
	};
}
