#pragma once

// A shot, as the demo game encodes it, and the test that decides what it hit.
//
// **This is a game rule and it lives in a game module on purpose.**
// `replication::Input::Bytes` is documented as "the game's own encoding", and
// `replication::Rewind` deliberately answers *where things were* and nothing
// else. A hit test inside either of them would be a game's idea of a weapon
// sitting in a network module, where every game that disagreed would have to
// work around it.
//
// So the split is: `replication` remembers, this decides, and `mono.server`
// joins them. Nothing here knows what a client is, a tick is, or a connection
// is — it is a ray, some spheres and an encoding.
//
// **Server-authoritative, which is the whole reason the ray crosses the wire
// rather than the hit.** A client that sent "I hit entity 42" would be a client
// that decides what it hit, and no amount of validation afterwards recovers
// from that. It sends where it aimed; the server decides.
//
// @tier L10 · shared

#include <engine/core/Bytes.hpp>
#include <engine/core/types/Ray.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::examples {

	// What a client sends when it shoots.
	//
	// @since v0.9
	struct Shot {
		// Where it was aimed from and along, in world space.
		core::Ray Aim;

		// How far it reaches, in metres.
		float Range = 100.0f;
	};

	// The longest a shot may reach.
	//
	// **A ceiling on a number a client chooses**, which is the whole of why it
	// is here: a range of a million metres is a client asking the server to test
	// its ray against the entire world, once per input, as often as it likes.
	// Refused rather than clamped, because a client sending one is not playing
	// the game.
	constexpr float MAXIMUM_SHOT_RANGE = 1000.0f;

	// Encodes a shot for `replication::Input::Bytes`.
	//
	// @param shot The shot.
	// @return The bytes.
	std::vector<std::byte> EncodeShot(const Shot &shot);

	// Decodes a shot, refusing anything that is not one.
	//
	// **Every field is hostile.** These bytes came from a client, and a client
	// is the one participant a server may not trust: refuses a non-finite
	// coordinate, a direction that is not unit length, a range past
	// `MAXIMUM_SHOT_RANGE`, and trailing bytes. A direction of zero would make
	// every distance along the ray the origin, so every entity at the origin
	// would be hit by every shot.
	//
	// @param bytes The payload.
	// @param out   Filled on success, left alone otherwise.
	// @return `false` on anything malformed.
	bool DecodeShot(std::span<const std::byte> bytes, Shot &out);

	// One candidate for a shot to hit.
	//
	// @since v0.9
	struct Target {
		ecs::Entity Entity;

		// Where it was when the shooter saw it — `replication::Rewind`'s answer
		// rather than where it is now, which is the entire point.
		core::Vector3 At;

		// How big it is, as a radius.
		//
		// **A sphere and not the box the part actually is.** A sphere test is
		// six multiplies and no orientation, and the difference between the two
		// at the sizes a projectile cares about is smaller than the tick the
		// rewind is already approximating. A box test is the right answer once
		// there is a reason for one; there is not yet, and pretending otherwise
		// would be precision the rest of the chain cannot support.
		float Radius = 0.5f;
	};

	// What a shot hit.
	//
	// @since v0.9
	struct Hit {
		// The entity, or an invalid one when nothing was hit.
		ecs::Entity Entity;

		// How far along the ray, in metres.
		float Distance = 0.0f;

		// Whether anything was hit at all.
		bool Struck = false;
	};

	// The nearest target a shot reaches.
	//
	// **Nearest rather than first, and the difference matters.** The candidates
	// arrive in whatever order a hash map walked them, so "the first one that
	// intersects" is a different answer on two machines with the same input —
	// and a server whose hit resolution depends on map ordering is one whose
	// recordings do not replay.
	//
	// @param shot    Where it was aimed.
	// @param targets What it might hit.
	// @return What it hit, or a miss.
	Hit NearestHit(const Shot &shot, std::span<const Target> targets);
}
