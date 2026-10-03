#pragma once
#include "ImageGraphObservations.hpp"

#include <engine/core/Name.hpp>

namespace studio::detail {
	// One preview request can span device frames. Playback remains owned by the timeline.
	struct ImageGraphComposerCadence {
		struct Identity {
			engine::core::Name Owner;
			std::string Output;
			uint64_t Revision = 0, InputRevision = 0;
			uint8_t RigidObservation = 0;
			bool operator==(const Identity &) const = default;
		};
		Identity Current;
		engine::imagegraph::FrameTime Frame{};
		std::optional<engine::imagegraph::FrameTime> Displayed;
		ImageGraphObservations Observations;
		bool Held = false, Playing = false, FrameProgress = false;
		// A frame-progress pulse belongs to the captured request. Its clearing on
		// the next UI draw cannot supersede device work already in flight.
		Identity RetainProgressPulse(Identity identity) const {
			if (!Held) return identity;
			auto withoutPulse = identity;
			withoutPulse.RigidObservation = Current.RigidObservation;
			if (withoutPulse == Current && ((identity.RigidObservation ^ Current.RigidObservation) & 5) == 0)
				identity.RigidObservation = Current.RigidObservation;
			return identity;
		}
		bool Matches(const Identity &identity) const {
			return Held && Current == identity;
		}
		engine::imagegraph::FrameTime Begin(
			const Identity &identity,
			engine::imagegraph::FrameTime playback,
			const ImageGraphObservations &observations,
			bool playing = false,
			bool frameProgress = false
		) {
			if (!Matches(identity)) {
				Current = identity;
				Frame = playback;
				Observations = observations;
				Playing = playing;
				FrameProgress = frameProgress;
				Held = true;
			}
			return Frame;
		}
		void Pending() {
			Held = true;
		}
		bool Complete(engine::imagegraph::FrameTime frame) {
			if (!Held || frame != Frame) return false;
			Displayed = frame;
			Held = false;
			return true;
		}
		void Cancel() {
			Held = false;
		}
	};
}
