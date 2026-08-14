#pragma once

// What a host and one of its clients say to each other while playing.
//
// Two messages, one in each direction, and both exist because the replicated
// state cannot carry them.
//
// **Down: which `Player` is yours.** `scene::LocalPlayer` has existed since
// v0.10 with a comment explaining that it is empty on a server and holds this
// viewer's player on a client - and it had no writer anywhere in the
// repository. So `Players.LocalPlayer` was nil on every client that ever ran,
// and with two clients in one world neither could tell which of the two
// characters was its own. It cannot be replicated, and that is not an oversight
// in `replication::LocalToTheClient`: a resource is one row in one world and
// the answer differs for every client watching it. Per-client state travels as
// a per-client message, over `replication::MessageKind::User` - which is
// exactly what that seam was reserved for.
//
// **Up: which way you are trying to walk.** The alternative was for a client to
// simulate its own character and submit the resulting `Transform` through
// `replication::SubmitState`, and that is the wrong trade for a first character
// controller: it puts a physics step on both ends, makes every disagreement a
// reconciliation problem, and hands a client the ability to state where its
// body is. Sending the *intent* keeps one simulation, and it is the same
// division `Server::ApplyInputs` already states for shooting - a client says
// where it aimed, never what it hit.
//
// **Here rather than in `replication`, because `replication` must not learn
// what a player is.** `game` is the highest module a client and a server both
// link, which makes it the only place one definition can serve both - and a
// second definition of a wire format is the class of bug rule 4 is about.
//
// @tier L10 · shared

#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::game {

	// The first byte of every message here, so a reader can refuse one it does
	// not know rather than parsing a payload that was never for it.
	//
	// **A tag even for the first message**, because both channels are shared:
	// the studio's edit stream already sends user messages over its own link,
	// and a client's input channel already carries `examples::Shot`. The day an
	// untagged payload is read as one of these is the day a wrong answer looks
	// like a working one.
	enum class PlayMessage : uint8_t {
		// Host to client: "this `Player` instance is yours."
		AssignPlayer = 1,

		// Client to host: "this is the way I am trying to move."
		Move = 2,
	};

	// Which player a client is looking through.
	//
	// @since v0.14
	struct JoinNotice {
		// The `Player` instance, in the authority's numbering.
		//
		// **The authority's handle is the client's handle**, which is a fact
		// about this engine's replication rather than an assumption: a replica
		// applies structure with the entity ids it was sent -
		// `replication::Replica` calls `SetParent` with the arriving handles -
		// so the two ends agree on numbering by construction. An engine that
		// remapped would need a lookup here instead.
		ecs::Entity Player;
	};

	// Which way a client is trying to move its own character.
	//
	// @since v0.14
	struct MoveInput {
		// The direction, in world space, as `scene::Humanoid::MoveDirection`
		// means it.
		//
		// **A direction and never a velocity**, so how fast the character walks
		// stays the server's `WalkSpeed` and is not something a client states.
		// The host normalises anyway - a client is not trusted to.
		core::Vector3 Direction;

		// Whether the jump key went down since the last submission.
		//
		// **An edge and not a hold**, so a client cannot hover by sending
		// "jumping" every tick: the humanoid only leaves the ground when the
		// host's own `Grounded` says it may.
		bool Jump = false;
	};

	// Packs a join notice.
	//
	// @param notice What to say.
	// @return The bytes to hand to a user-message send.
	std::vector<std::byte> EncodeJoinNotice(const JoinNotice &notice);

	// Unpacks a join notice.
	//
	// **Returns false for anything that is not one**, including a message with
	// another tag, which is what makes it safe to feed every user message
	// through this and ignore the ones it refuses.
	//
	// @param message The bytes that arrived.
	// @param out     Filled on success.
	// @return `false` when the message is not a join notice.
	bool DecodeJoinNotice(std::span<const std::byte> message, JoinNotice &out);

	// Writes a move onto the character a player is allowed to move.
	//
	// **The lookup is the whole of the security.** A client names nothing - it
	// says only which way it is trying to walk - so the body the intent lands on
	// is the one the host assigned to that connection, and a client that sent a
	// hundred moves still moves one character.
	//
	// **Here rather than in each host, because there are two of them.** A
	// dedicated server applies this from `replication::Listener::Inputs`, and
	// the studio applies it from a `PlayLink` with no socket in the middle; two
	// copies of "which field does a move touch" is the shape that drifts and
	// then only drifts in the editor, which is the worst place to notice.
	//
	// The jump is latched rather than assigned, exactly as
	// `scene::UpdateCharacterControl` latches it: a jump arriving between two
	// simulation ticks must survive until the step reads it.
	//
	// @param store  The world holding the player.
	// @param player The `Player` instance.
	// @param move   What they asked for, already normalised by the decoder.
	// @return `false` when that player has no character to move.
	bool ApplyMoveInput(ecs::Store &store, ecs::Entity player, const MoveInput &move);

	// Packs a move input.
	//
	// @param input What the client is trying to do.
	// @return The bytes to submit.
	std::vector<std::byte> EncodeMoveInput(const MoveInput &input);

	// Unpacks a move input.
	//
	// **Refuses a payload of any other length**, which is what keeps it apart
	// from `examples::Shot` on the same channel: a shot is seven floats and
	// untagged, and a decoder that only checked the first byte would eventually
	// read one as a move.
	//
	// **The direction is not trusted.** A client may send any three floats; the
	// host normalises what arrives and drops anything that is not finite, so
	// the worst a bad client achieves is walking at its own `WalkSpeed` in a
	// direction of its choosing - which is what a good one does too.
	//
	// @param bytes The payload.
	// @param out   Filled on success.
	// @return `false` when the payload is not a move input.
	bool DecodeMoveInput(std::span<const std::byte> bytes, MoveInput &out);
}
