#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Volume.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace engine::scene {
	namespace {
		// A field-sourced volume's density is scaled by its latest sample.
		float FieldFactor(const ecs::Store &store, ecs::Entity entity) {
			const VolumeFieldSample *sample = store.Get<VolumeFieldSample>(entity);
			return sample != nullptr ? std::max(sample->Factor, 0.0f) : 1.0f;
		}

		VolumeState Resolved(
			const ecs::Store &store, ecs::Entity entity, const Volume &volume, const Transform &transform
		) {
			return {
				.Frame = transform.Frame,
				.Colour = volume.Colour,
				.HalfExtent = volume.HalfExtent,
				.Density = std::max(volume.Density, 0.0f) * FieldFactor(store, entity),
				.Extinction = std::max(volume.Extinction, 0.0f),
				.Falloff = std::clamp(volume.Falloff, 0.0f, 1.0f),
				.NoiseScale = std::max(volume.NoiseScale, 0.001f),
				.NoiseStrength = std::clamp(volume.NoiseStrength, 0.0f, 1.0f),
				.Steps = std::clamp(volume.Steps, 1u, 64u),
				.ShadowSteps = std::clamp(volume.ShadowSteps, 1u, 32u),
				.Seed = volume.Seed,
				.Shape = volume.Shape,
				.Enabled = true,
			};
		}

		bool Valid(const Volume &volume) {
			return volume.Enabled && volume.HalfExtent.X > 0.0f && volume.HalfExtent.Y > 0.0f &&
				   volume.HalfExtent.Z > 0.0f;
		}
	}

	size_t ResolveVolumes(const ecs::Store &store, std::span<VolumeState> out) {
		size_t count = 0;
		auto &mutableStore = const_cast<ecs::Store &>(store);
		mutableStore.Each<const Volume, const Transform>(
			[&](ecs::Entity entity, const Volume &volume, const Transform &transform) {
				if (count == out.size() || !Valid(volume)) {
					return;
				}
				out[count++] = Resolved(store, entity, volume, transform);
			}
		);
		return count;
	}

	size_t ResolveVolumes(
		const ecs::Store &store,
		const core::Vector3 &eye,
		std::span<const core::AABB> receivers,
		std::span<VolumeState> out
	) {
		struct Candidate {
			VolumeState State;
			float ReceiverDistance = 0.0f;
			float ReceiverCentreDistance = 0.0f;
			float EyeDistance = 0.0f;
		};
		auto before = [](const Candidate &left, const Candidate &right) {
			if (left.ReceiverDistance != right.ReceiverDistance)
				return left.ReceiverDistance < right.ReceiverDistance;
			if (left.ReceiverCentreDistance != right.ReceiverCentreDistance)
				return left.ReceiverCentreDistance < right.ReceiverCentreDistance;
			return left.EyeDistance < right.EyeDistance;
		};
		static thread_local std::vector<Candidate> selected;
		selected.clear();
		if (selected.capacity() < out.size()) selected.reserve(out.size());
		auto &mutableStore = const_cast<ecs::Store &>(store);
		mutableStore.Each<const Volume, const Transform>([&](ecs::Entity entity,
															 const Volume &volume,
															 const Transform &transform) {
			if (!Valid(volume)) return;
			const float radius = std::max({volume.HalfExtent.X, volume.HalfExtent.Y, volume.HalfExtent.Z});
			Candidate candidate;
			candidate.State = Resolved(store, entity, volume, transform);
			candidate.EyeDistance = std::max((transform.Frame.Position - eye).Magnitude() - radius, 0.0f);
			candidate.ReceiverDistance = candidate.EyeDistance;
			candidate.ReceiverCentreDistance = candidate.EyeDistance;
			if (!receivers.empty()) {
				// Squared distances in the loop and one root after: the root is
				// monotonic and correctly rounded, so the minimum is the same
				// number the per-receiver roots gave.
				const core::Vector3 at = transform.Frame.Position;
				float nearestSquared = std::numeric_limits<float>::infinity();
				float centreSquared = std::numeric_limits<float>::infinity();
				for (const core::AABB &receiver : receivers) {
					nearestSquared =
						std::min(nearestSquared, (at - receiver.ClosestPoint(at)).MagnitudeSquared());
					centreSquared = std::min(centreSquared, (at - receiver.Centre()).MagnitudeSquared());
				}
				candidate.ReceiverDistance = std::max(std::sqrt(nearestSquared) - radius, 0.0f);
				candidate.ReceiverCentreDistance = std::sqrt(centreSquared);
			}
			const auto position =
				std::find_if(selected.begin(), selected.end(), [&](const Candidate &current) {
					return before(candidate, current);
				});
			if (position == selected.end() && selected.size() == out.size()) return;
			selected.insert(position, candidate);
			if (selected.size() > out.size()) selected.pop_back();
		});
		for (size_t index = 0; index < selected.size(); index++)
			out[index] = selected[index].State;
		return selected.size();
	}

}
