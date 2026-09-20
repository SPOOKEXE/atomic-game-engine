#pragma once

#include <engine/core/Name.hpp>
#include <engine/scene/DrawInstance.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::render {

	// Maximum entity rows retained for one view submission.
	inline constexpr size_t MAX_VISIBILITY_OBSERVATIONS = 4096;

	// Renderer visibility outcome for one draw instance.
	enum class VisibilityState : uint8_t {
		Unavailable,
		SubmittedOpaqueOrMasked,
		SubmittedBlended,
		FrustumCulled,
		DepthOccluded,
		GpuIndirectCandidate
	};
	// Render decision that produced a visibility outcome.
	enum class VisibilityCause : uint8_t {
		Unavailable,
		OpaqueOrMaskedDraw,
		BlendedDraw,
		Frustum,
		ExactDepth,
		GpuIndirectEligibility
	};

	// Returns a stable text name for a visibility state.
	const char *Describe(VisibilityState state);
	// Returns a stable text name for a visibility cause.
	const char *Describe(VisibilityCause cause);

	// Visibility result recorded for one entity in one world.
	struct VisibilityObservation {
		// World containing the observed entity.
		core::Name World;
		// Entity handle within World.
		uint64_t Entity = 0;
		// Last visibility outcome seen for this entity.
		VisibilityState State = VisibilityState::Unavailable;
		// Rendering decision that set State.
		VisibilityCause Cause = VisibilityCause::Unavailable;
	};

	// An owned copy of one completed submission. Invalid snapshots have no rows.
	struct VisibilitySnapshot {
		// Render frame that produced the snapshot.
		uint64_t Frame = 0;
		// Renderer view slot that produced the snapshot.
		size_t ViewSlot = 0;
		// World rendered by the view.
		core::Name World;
		// False when Begin was not completed for this submission.
		bool Valid = false;
		// Rows omitted after fixed storage filled.
		size_t Dropped = 0;
		// Whether Dropped is exact rather than a lower bound.
		bool DroppedExact = true;
		// Per-entity observations in first-seen order.
		std::vector<VisibilityObservation> Observations;
	};

	// Fixed rows and an open-addressed index keep duplicate variants attached to
	// their source entity without a quadratic search.
	class VisibilityObservations {
	  public:
		// Starts recording one view submission.
		void Begin(uint64_t frame, size_t viewSlot, core::Name world);
		// Marks the active submission unusable.
		void Invalidate();
		// Creates or finds the row for an encountered draw instance.
		void Observe(const scene::DrawInstance &instance);
		// Marks an instance submitted to an opaque or blended draw.
		void Submitted(const scene::DrawInstance &instance);
		// Marks an instance eligible for indirect GPU submission.
		void GpuIndirectCandidate(const scene::DrawInstance &instance);
		// Marks an instance rejected by the view frustum.
		void FrustumCulled(const scene::DrawInstance &instance);
		// Copies the completed visibility record for inspection.
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
