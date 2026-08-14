#include <engine/audio/Spatial.hpp>

#include <algorithm>
#include <cmath>

namespace engine::audio {
	namespace {
		constexpr float QUARTER_TURN = 1.57079632679489661923f;
	}

	StereoGain PanGain(float pan) {
		const float clamped = std::clamp(pan, -1.0f, 1.0f);

		// Map -1..+1 onto 0..90 degrees, then take cosine and sine. At the
		// centre both are 1/root(2), so the summed power is the same as at
		// either extreme - which is what stops a swept sound sagging as it
		// passes in front.
		const float angle = (clamped + 1.0f) * 0.5f * QUARTER_TURN;
		return StereoGain{.Left = std::cos(angle), .Right = std::sin(angle)};
	}

	float DistanceGain(float distance, const EmitterPlacement &placement) {
		const float start = std::max(0.0f, placement.FalloffStart);
		const float end = std::max(start, placement.FalloffEnd);

		if (distance <= start) {
			return 1.0f;
		}
		if (distance >= end || end <= start) {
			// Silent past the end, and exactly zero rather than nearly. A tail
			// that never quite reaches zero keeps every sound in the world
			// mixing for ever.
			return 0.0f;
		}

		// Inverse-square between the two, normalised so it hits zero at the
		// end. `start / distance` squared is the physical falloff; subtracting
		// its value at the end and rescaling is what pins the far end to zero
		// without changing the shape near the listener.
		const float here = (start <= 0.0f) ? 0.0f : (start / distance) * (start / distance);
		const float there = (start <= 0.0f) ? 0.0f : (start / end) * (start / end);
		if (start <= 0.0f) {
			// No full-volume radius at all: fall back to a straight ramp, which
			// is the only thing left when there is no reference distance to
			// take a ratio against.
			return 1.0f - (distance - start) / (end - start);
		}
		return std::clamp((here - there) / (1.0f - there), 0.0f, 1.0f);
	}

	float DistanceBetween(const ListenerPose &listener, const EmitterPlacement &placement) {
		const float dx = placement.X - listener.X;
		const float dy = placement.Y - listener.Y;
		const float dz = placement.Z - listener.Z;
		return std::sqrt(dx * dx + dy * dy + dz * dz);
	}

	StereoGain Place(const ListenerPose &listener, const EmitterPlacement &placement) {
		const float dx = placement.X - listener.X;
		const float dy = placement.Y - listener.Y;
		const float dz = placement.Z - listener.Z;
		const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);

		const float attenuation = DistanceGain(distance, placement);
		if (attenuation <= 0.0f) {
			return StereoGain{.Left = 0.0f, .Right = 0.0f};
		}

		// **A sound on top of the listener is centred, not panned by whatever
		// the normalised zero vector happened to be.** Dividing by a distance
		// of zero is the obvious crash here, and the less obvious one is a
		// direction that flips wildly as somebody walks through the emitter.
		float pan = 0.0f;
		if (distance > 0.0001f) {
			// The component of the direction along the listener's right vector.
			// Already in -1..1 for unit vectors, so no further normalising -
			// and clamped anyway, because a caller's "unit" vector is only as
			// unit as whatever produced it.
			pan = std::clamp(
				(dx * listener.RightX + dy * listener.RightY + dz * listener.RightZ) / distance, -1.0f, 1.0f
			);
		}

		const StereoGain placed = PanGain(pan);
		return StereoGain{.Left = placed.Left * attenuation, .Right = placed.Right * attenuation};
	}
}
