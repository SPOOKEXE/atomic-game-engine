#pragma once

// Turning "where is this sound and where am I" into two numbers.
//
// **Two numbers, because the mixer is stereo.** Everything spatialisation does
// here comes out as a left gain and a right gain, and that is the whole of the
// interface - no filters, no head model, no occlusion. A binaural renderer and
// a distance-dependent low-pass are both real things this will eventually want,
// and neither is pretended to exist.
//
// **Pure arithmetic with no state**, so it is a free function rather than a
// class. It runs per emitter per block on the device thread, and a call that
// cannot allocate and cannot fail is the kind that belongs there.
//
// @tier L12 · client

#include <engine/audio/Graph.hpp>

namespace engine::audio {

	// What an emitter's signal should be multiplied by, per channel.
	//
	// @since v0.9
	struct StereoGain {
		// What the left channel is multiplied by.
		float Left = 1.0f;

		// What the right channel is multiplied by.
		float Right = 1.0f;
	};

	// Equal-power panning from a position between hard left and hard right.
	//
	// **Equal power rather than linear**, and the difference is audible. A
	// linear pan - left = 1-p, right = p - drops about 3 dB in the middle,
	// so a sound swept across the front sags as it passes the centre. Taking
	// the cosine and sine of a quarter turn keeps `L² + R²` constant, which is
	// what the ear tracks.
	//
	// @param pan -1 hard left, 0 centre, +1 hard right. Clamped.
	// @return The per-channel gains.
	StereoGain PanGain(float pan);

	// How loud something is at a distance.
	//
	// **Two distances rather than a rolloff exponent**, because two distances
	// are what somebody placing a sound can reason about: it is full volume
	// within `FalloffStart` and silent past `FalloffEnd`.
	//
	// Between them the curve is inverse-square-ish rather than linear - sound
	// intensity falls with the square of distance in the real world, and a
	// linear ramp is why a sound can seem to switch off as you walk away from
	// it. It is normalised to reach exactly zero at `FalloffEnd`, so nothing
	// keeps mixing at an inaudible level for ever, which is otherwise the
	// standing cost of a physically honest curve.
	//
	// @param distance How far away it is.
	// @param placement Its falloff distances.
	// @return A gain from 1 down to 0.
	float DistanceGain(float distance, const EmitterPlacement &placement);

	// Where a listener hears an emitter.
	//
	// @param listener Where the ear is and which way it faces.
	// @param placement Where the sound is and how far it carries.
	// @return The per-channel gains, distance and direction together.
	// @since v0.9
	StereoGain Place(const ListenerPose &listener, const EmitterPlacement &placement);

	// How far apart they are.
	//
	// @param listener Where the ear is.
	// @param placement Where the sound is.
	// @return The distance.
	float DistanceBetween(const ListenerPose &listener, const EmitterPlacement &placement);
}
