#pragma once

// What the server and a client actually say to each other.
//
// One message set, in one place, read and written by one pair of functions.
// The alternative — each half framing its own — is how two builds end up
// disagreeing about a field, and the disagreement surfaces as a desync a long
// way from its cause. `net::Packet` makes the same argument one layer down.
//
// **A tick is the unit of agreement.** Not a timestamp and not a packet
// sequence: the server stamps what it sends with the tick it describes, a
// client acknowledges the last tick it applied, and prediction replays the
// inputs after that tick. Agreeing on wall time instead would need two clocks
// to agree, and they do not.
//
// **Names cross, ids do not.** A `ComponentId` is a dense counter assigned in
// registration order and means something else in the other process, so every
// component is named on the wire and resolved once per message rather than once
// per entity. An `ecs::Entity` *is* carried as its index and generation, and
// that is deliberate: reproducing the directory exactly is what makes a handle
// mean the same thing on both sides.
//
// @tier L12 · shared

#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Entity.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace engine::replication {

	// What a message is.
	//
	// @since v0.3
	enum class MessageKind : uint8_t {
		// Server to client: one piece of a full world snapshot.
		//
		// Chunked because a world is megabytes and a datagram is about twelve
		// hundred bytes. Reassembled before it is applied, so a client never
		// sees a half-world.
		SnapshotChunk,

		// Server to client: what changed in one tick.
		Delta,

		// Server to client: which entities a client holds at all.
		//
		// **Its own kind because it is the only thing said exactly once.** A
		// component value that goes missing is offered again next tick by the
		// unconfirmed set; an entity that was created, destroyed or forgotten is
		// announced once and the server's known set has moved on — so a lost one
		// is a client permanently out of step with a server that is content. A
		// kind of its own is what puts it on the reliable channel, which is the
		// one thing here that redelivers until the far side has it. See
		// `ChannelFor`.
		Structure,

		// Client to server: what the player did, and the tick it was for.
		Input,

		// Client to server: the last tick applied in full.
		//
		// What lets the server stop resending and what prediction replays
		// from.
		Applied,
	};

	// Returns a stable, human-readable name for a message kind.
	//
	// @param kind The kind to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(MessageKind kind);

	// The wire format version.
	//
	// **Refused when unknown, never negotiated downward** — the same rule
	// `net::Packet` states, for the same reason: a server speaking an old
	// version to an old client is a server running two protocols, and the
	// second one is the one nobody tests.
	//
	// 2 at v0.5: creations and destructions moved out of `Delta` and joined
	// forgets in `Structure`, so a version 1 peer would read a delta's component
	// count out of the bytes that used to be its creation list.
	inline constexpr uint16_t PROTOCOL_VERSION = 2;

	// One piece of a snapshot.
	//
	// @since v0.3
	struct SnapshotChunk {
		// The tick the snapshot describes.
		uint64_t Tick = 0;

		// Total bytes in the whole snapshot, so a receiver can size its buffer
		// once and refuse a chunk that claims to run past the end.
		uint32_t TotalBytes = 0;

		// Where this piece starts.
		uint32_t Offset = 0;

		// This piece.
		std::vector<std::byte> Bytes;
	};

	// One component's changed rows, for one tick.
	//
	// Runs rather than rows: `ecs::Store::EachChangedBatch` yields adjacent
	// changed rows as a block, which is what lets a delta copy values as a
	// memcpy per run rather than once per entity.
	//
	// @since v0.3
	struct ComponentDelta {
		// Which component, by name. An id means something else over there.
		core::Name Component;

		// The entities whose values follow, in order.
		std::vector<ecs::Entity> Entities;

		// The values, one per entity, in that order — written through the
		// component's own `TypeDescriptor`, so a type with a custom serialiser
		// crosses correctly rather than as its object representation.
		std::vector<std::byte> Values;
	};

	// What moved in one tick.
	//
	// **Values only.** Which entities exist is `Structure`'s, because the two
	// need different delivery: a value is superseded by the next tick and a
	// structural change never is.
	//
	// @since v0.3
	struct Delta {
		// The tick this describes. A client applies deltas in tick order and
		// ignores one it has already applied.
		uint64_t Tick = 0;

		// The tick the sender believes the receiver last applied. Carried so a
		// client can tell a gap from a reorder without keeping its own history.
		uint64_t Baseline = 0;

		// What moved, per component.
		std::vector<ComponentDelta> Components;
	};

	// Which entities a client holds, and the three ways that changes.
	//
	// **Carries no tick gate on the receiving side, and that is the point.** It
	// rides the reliable channel, so it arrives in the order it was sent and a
	// resend of one whose datagram was lost turns up long after the tick it was
	// decided at. Refusing that as stale is exactly how a creation goes missing
	// for the life of a connection.
	//
	// @since v0.5
	struct Structure {
		// The tick this was decided at. Carried for logs and for the order the
		// three lists were computed in; it is not what decides whether the
		// message is applied.
		uint64_t Tick = 0;

		// Entities the client does not have yet and should create, at the
		// sender's exact index and generation.
		std::vector<ecs::Entity> Created;

		// Entities that no longer exist anywhere.
		std::vector<ecs::Entity> Destroyed;

		// Entities the client may no longer see.
		//
		// **Not destroyed**, and the distinction matters: a client that
		// conflated the two would delete an entity that is still there and then
		// be wrong about the world the moment it came back into view.
		std::vector<ecs::Entity> Forgotten;
	};

	// What a player did in one tick.
	//
	// Opaque bytes, because this layer does not know what a game's input is and
	// must not: a module that knew would need changing for every game.
	//
	// @since v0.3
	struct Input {
		// The tick the input was produced for.
		uint64_t Tick = 0;

		// The game's own encoding.
		std::vector<std::byte> Bytes;
	};

	// The last tick a client applied in full.
	//
	// @since v0.3
	struct Applied {
		// The tick.
		uint64_t Tick = 0;
	};

	// Writes a message, including its kind and the protocol version.
	//
	// @param writer Where the bytes go.
	// @param chunk  The chunk to write.
	// @since v0.3
	void WriteMessage(core::ByteWriter &writer, const SnapshotChunk &chunk);

	// Writes a delta.
	//
	// @param writer Where the bytes go.
	// @param delta  The delta to write.
	// @since v0.3
	void WriteMessage(core::ByteWriter &writer, const Delta &delta);

	// Writes a structural change.
	//
	// @param writer    Where the bytes go.
	// @param structure What the client should now hold.
	// @since v0.5
	void WriteMessage(core::ByteWriter &writer, const Structure &structure);

	// Writes an input.
	//
	// @param writer Where the bytes go.
	// @param input  The input to write.
	// @since v0.3
	void WriteMessage(core::ByteWriter &writer, const Input &input);

	// Writes an acknowledgement.
	//
	// @param writer  Where the bytes go.
	// @param applied The tick applied.
	// @since v0.3
	void WriteMessage(core::ByteWriter &writer, const Applied &applied);

	// What a successful read produced.
	//
	// A tagged union by hand rather than a variant: exactly one of the bodies
	// below is meaningful and `Kind` says which, which is the shape the
	// dispatch on the other side wants anyway.
	//
	// @since v0.3
	struct Message {
		// Which body is filled in.
		MessageKind Kind = MessageKind::Applied;

		// Meaningful when `Kind` is `SnapshotChunk`.
		SnapshotChunk Chunk;

		// Meaningful when `Kind` is `Delta`.
		replication::Delta Delta;

		// Meaningful when `Kind` is `Structure`.
		replication::Structure Structure;

		// Meaningful when `Kind` is `Input`.
		replication::Input Input;

		// Meaningful when `Kind` is `Applied`.
		replication::Applied Applied;
	};

	// Reads a message, refusing anything that is not one.
	//
	// Refuses an unknown version, a kind outside the enum, a count that runs
	// past the buffer, and a truncated body. **A kind byte is range-checked
	// before the cast** — casting it anyway produces a value no switch handles,
	// and every `Describe` and dispatch downstream then reads something the
	// type says cannot exist.
	//
	// @param reader  The bytes to parse.
	// @param message Filled in on success, untouched otherwise — so a caller
	//                reusing one across a poll loop cannot act on a mixture of
	//                the last good message and a bad one.
	// @return `false` on anything malformed. Drop it and count it.
	// @since v0.3
	bool ReadMessage(core::ByteReader &reader, Message &message);

	// The most entities or components one message may carry.
	//
	// A bound on what a malformed count can make a receiver allocate. Far above
	// any real tick's traffic and far below anything that would hurt — without
	// it, four bytes from a peer are an out-of-memory kill.
	inline constexpr uint32_t MAXIMUM_ENTRIES = 1u << 20u;
}
