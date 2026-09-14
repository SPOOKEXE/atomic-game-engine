#include <engine/render/VisibilityObservation.hpp>

namespace engine::render {

	const char *Describe(VisibilityState state) {
		switch (state) {
		case VisibilityState::Unavailable:
			return "unavailable";
		case VisibilityState::SubmittedOpaqueOrMasked:
			return "submitted-opaque-or-masked";
		case VisibilityState::SubmittedBlended:
			return "submitted-blended";
		case VisibilityState::FrustumCulled:
			return "frustum-culled";
		case VisibilityState::DepthOccluded:
			return "depth-occluded";
		case VisibilityState::GpuIndirectCandidate:
			return "gpu-indirect-candidate";
		}
		return "unavailable";
	}

	const char *Describe(VisibilityCause cause) {
		switch (cause) {
		case VisibilityCause::Unavailable:
			return "unavailable";
		case VisibilityCause::OpaqueOrMaskedDraw:
			return "opaque-or-masked-draw";
		case VisibilityCause::BlendedDraw:
			return "blended-draw";
		case VisibilityCause::Frustum:
			return "frustum";
		case VisibilityCause::ExactDepth:
			return "exact-depth";
		case VisibilityCause::GpuIndirectEligibility:
			return "gpu-indirect-eligibility";
		}
		return "unavailable";
	}

	void VisibilityObservations::Begin(uint64_t frame, size_t viewSlot, core::Name world) {
		Frame = frame;
		ViewSlot = viewSlot;
		ViewWorld = world;
		Active = true;
		Count = 0;
		OverflowCount = 0;
		Dropped = 0;
		DroppedExact = true;
		Index.fill(-1);
	}

	void VisibilityObservations::Invalidate() {
		Active = false;
		Count = 0;
		OverflowCount = 0;
		Dropped = 0;
		DroppedExact = true;
		Index.fill(-1);
		ViewWorld = {};
		Frame = 0;
		ViewSlot = 0;
	}

	size_t VisibilityObservations::Hash(core::Name world, uint64_t entity) {
		uint64_t key = (static_cast<uint64_t>(world.Id()) << 32) ^ entity;
		key ^= key >> 33;
		key *= 0xff51afd7ed558ccdULL;
		key ^= key >> 33;
		return static_cast<size_t>(key);
	}

	VisibilityObservation *VisibilityObservations::Find(const scene::DrawInstance &instance, bool create) {
		if (!Active || instance.Source == 0) return nullptr;
		const core::Name world = instance.SourceWorld.IsValid() ? instance.SourceWorld : ViewWorld;
		size_t slot = Hash(world, instance.Source) % Index.size();
		for (size_t probe = 0; probe < Index.size(); probe++) {
			const int32_t rowIndex = Index[slot];
			if (rowIndex < 0) {
				if (!create) return nullptr;
				if (Count == Rows.size()) {
					for (size_t omitted = 0; omitted < OverflowCount; omitted++)
						if (Overflow[omitted].World == world && Overflow[omitted].Entity == instance.Source)
							return nullptr;
					if (OverflowCount < Overflow.size())
						Overflow[OverflowCount++] = VisibilityObservation{world, instance.Source};
					else
						DroppedExact = false;
					if (DroppedExact) Dropped++;
					return nullptr;
				}
				Rows[Count] = VisibilityObservation{world, instance.Source};
				Index[slot] = static_cast<int32_t>(Count);
				return &Rows[Count++];
			}
			VisibilityObservation &row = Rows[static_cast<size_t>(rowIndex)];
			if (row.World == world && row.Entity == instance.Source) return &row;
			slot = (slot + 1) % Index.size();
		}
		return nullptr;
	}

	void VisibilityObservations::Observe(const scene::DrawInstance &instance) {
		(void)Find(instance, true);
	}

	void VisibilityObservations::Submitted(const scene::DrawInstance &instance) {
		VisibilityObservation *const row = Find(instance, false);
		if (row == nullptr) return;
		if (scene::IsTransparent(instance)) {
			row->State = VisibilityState::SubmittedBlended;
			row->Cause = VisibilityCause::BlendedDraw;
		} else {
			row->State = VisibilityState::SubmittedOpaqueOrMasked;
			row->Cause = VisibilityCause::OpaqueOrMaskedDraw;
		}
	}
	void VisibilityObservations::GpuIndirectCandidate(const scene::DrawInstance &instance) {
		VisibilityObservation *const row = Find(instance, false);
		if (row == nullptr || row->State != VisibilityState::Unavailable) return;
		row->State = VisibilityState::GpuIndirectCandidate;
		row->Cause = VisibilityCause::GpuIndirectEligibility;
	}

	void VisibilityObservations::FrustumCulled(const scene::DrawInstance &instance) {
		VisibilityObservation *const row = Find(instance, false);
		if (row == nullptr || row->State == VisibilityState::SubmittedOpaqueOrMasked ||
			row->State == VisibilityState::SubmittedBlended)
			return;
		row->State = VisibilityState::FrustumCulled;
		row->Cause = VisibilityCause::Frustum;
	}

	VisibilitySnapshot VisibilityObservations::Snapshot() const {
		VisibilitySnapshot snapshot;
		if (!Active) return snapshot;
		snapshot.Frame = Frame;
		snapshot.ViewSlot = ViewSlot;
		snapshot.World = ViewWorld;
		snapshot.Valid = true;
		snapshot.Dropped = Dropped;
		snapshot.DroppedExact = DroppedExact;
		snapshot.Observations.assign(Rows.begin(), Rows.begin() + Count);
		return snapshot;
	}
}
