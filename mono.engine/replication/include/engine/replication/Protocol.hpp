#pragma once

// @tier L12 · shared

#include <engine/assets/ContentHash.hpp>
#include <engine/assets/Signature.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Entity.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace engine::replication {

	// What a message is.
	//
	// @since v0.3
	enum class MessageKind : uint8_t {
		SnapshotChunk,

		Delta,

		Structure,

		Input,

		Applied,

		// @since v0.9
		Identify,

		// A message this module does not read.
		//
		// **The seam that stops the next feature becoming a fourth session
		// type.** A connected, admitted, encrypted, reliable link between two
		// processes is expensive to build and this module already has one; a
		// caller that needs to carry something else between the same two ends —
		// a game's remote call, the studio's edit stream — should not have to
		// build a second one beside it.
		//
		// The payload is opaque here and must stay opaque. A `replication` that
		// parsed a user message would be a `replication` that knows what a
		// studio edit is.
		//
		// @since v0.13
		User,

		// The server's digests over state a client has already acknowledged.
		//
		// @since v0.15
		GroupSignatures,

		// A client answering one of those, naming the groups it disagrees with.
		//
		// @since v0.15
		Disputed,
	};

	// Returns a stable, human-readable name for a message kind.
	//
	// @param kind The kind to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(MessageKind kind);

	// A client proving which identity it holds.
	//
	// @since v0.9
	struct Identify {
		// The public half of the identity being claimed.
		assets::PublicKey Key;

		// The claim itself, signed by the private half.
		//
		// **A key alone proves nothing**, which is the whole reason this is two
		// fields: anybody can copy a public key off the wire and send it. What
		// the signature covers is the handshake this message belongs to, so a
		// replay from another session verifies against the wrong transcript.
		assets::SignatureBytes Signature;
	};

	// The wire format version.
	//
	// Unknown versions are refused; wire changes require a version bump.
	inline constexpr uint16_t PROTOCOL_VERSION = 7;

	// Which half of a join a snapshot chunk belongs to.
	//
	// **A join is two blobs because it is two cursors.** A snapshot is one
	// `ecs::Store::Save` chunked across ticks in whatever order the store wrote
	// its archetypes, and no ordering hook reaches inside a byte cursor — so
	// "these entities first" is a second snapshot finished before the first byte
	// of the world's goes out, and this is the field that says which one a chunk
	// is part of. See `Authority::SetPreface`.
	//
	// @since v0.15
	enum class SnapshotStage : uint8_t {
		// The entities a host asked for ahead of the world.
		//
		// Applied as an overlay rather than authoritatively, because it is a
		// slice of a world and not the whole of one: a receiver that swept
		// everything this blob failed to mention would empty the world it is
		// about to be sent again.
		Preface,

		// The whole of what a client may see, and the blob that ends the join.
		World,
	};

	// One piece of a snapshot.
	//
	// @since v0.3
	struct SnapshotChunk {
		// Which of the join's two blobs this is part of.
		//
		// **Part of the buffer's identity at the far side, not a label.** Both
		// blobs are stamped with the tick they were taken at, so two of the same
		// length would otherwise assemble into one another — the same failure
		// `Offset` exists to prevent, one level up.
		//
		// @since v0.15
		SnapshotStage Stage = SnapshotStage::World;

		// The tick the whole snapshot was taken at.
		//
		// Carried on every chunk rather than only the first, so a receiver can
		// tell a late chunk of a superseded snapshot from one it still wants.
		uint64_t Tick = 0;

		// How long the assembled snapshot is, so a receiver can size its buffer
		// once and refuse a total it will not accept before any of it arrives.
		uint32_t TotalBytes = 0;

		// Where this chunk starts within that total.
		//
		// **Explicit rather than implied by arrival order**, because chunks
		// travel on a channel that may reorder them — an appending receiver
		// would assemble a snapshot out of order and never know.
		uint32_t Offset = 0;

		// This chunk's bytes.
		std::vector<std::byte> Bytes;
	};

	// One component's changed rows, for one tick.
	//
	// @since v0.3
	struct ComponentDelta {
		// Which component this is, by name.
		//
		// **A name and not an id**, which is `AGENTS.md` rule 4 on the wire: two
		// processes register components in whatever order their code ran, so an
		// index means different things at each end.
		core::Name Component;

		// The rows that changed, in the order their values are packed.
		std::vector<ecs::Entity> Entities;

		// Those rows' values, back to back at the component's wire stride.
		//
		// **Parallel to `Entities` rather than interleaved with it**, so the
		// values are one memcpy per run of adjacent rows — which is what
		// `EachChangedBatch` yields and the reason it yields runs at all.
		std::vector<std::byte> Values;
	};

	// What moved in one tick.
	//
	// @since v0.3
	struct Delta {
		// The tick this describes.
		uint64_t Tick = 0;

		// The tick this is a difference against.
		//
		// **What makes a lost datagram survivable.** A delta built from one
		// tick's dirty bits describes only that tick, so an entity that moved on
		// a tick nobody received and then went still would be wrong on that
		// client for ever. The baseline says what the sender believes the
		// receiver already has, and anything unconfirmed since is resent.
		uint64_t Baseline = 0;

		// @since v0.5
		uint16_t Part = 0;

		// @since v0.5
		bool Final = true;

		// One entry per component that moved.
		std::vector<ComponentDelta> Components;
	};

	// Which entities a client holds, and the three ways that changes.
	//
	// @since v0.5
	struct Structure {
		// The tick these changes belong to.
		uint64_t Tick = 0;

		// Entities the client should now hold.
		//
		// Carried at the sender's exact index *and* generation, because a handle
		// stored inside a component has to mean the same entity at both ends.
		std::vector<ecs::Entity> Created;

		// Entities that no longer exist anywhere.
		std::vector<ecs::Entity> Destroyed;

		// Entities that still exist and this client can no longer see.
		//
		// **Never merged with `Destroyed`, and that is the point of having
		// three.** A client told to destroy something it merely lost sight of
		// would delete a thing that is still there and then be wrong about it
		// the moment it came back into view.
		std::vector<ecs::Entity> Forgotten;
	};

	// What a player did in one tick.
	//
	// @since v0.3
	struct Input {
		// The tick the player made it on, which is what lets the server retire
		// exactly the inputs it consumed and leave the rest to be replayed.
		uint64_t Tick = 0;

		// The input itself, opaque here.
		//
		// **This module does not know what a game's input is**, and reading it
		// would be `replication` knowing what a component means — the same line
		// `ComponentDelta::Values` is on.
		std::vector<std::byte> Bytes;
	};

	// Something this module carries and does not read.
	//
	// @since v0.13
	struct User {
		// The payload, whatever it is.
		//
		// **Reliable and ordered**, because that is the only promise worth
		// making about a message nobody here understands: an unreliable one
		// would be a feature every caller has to re-implement the recovery for,
		// and this module already has a reliable channel.
		std::vector<std::byte> Bytes;
	};

	// One group of replicated state, and the digest over it.
	//
	// @since v0.15
	struct AuditGroup {
		// Which group of *this audit* this is, counted from zero.
		//
		// A label for the answer to name rather than a place in anything the
		// two ends keep: the entities are listed below, so a group needs no
		// identity outside the message it arrived in.
		uint32_t Group = 0;

		// The entities the sender hashed, in the order it hashed them.
		//
		// **Listed rather than derived from the group number, and that is what
		// makes the comparison exact.** The sender audits only the rows this
		// client has already acknowledged, and a receiver working the
		// membership out for itself would include a row that is merely in
		// flight — reporting a disagreement about a value that is on its way.
		std::vector<ecs::Entity> Entities;

		// The digest of those entities' replicated values.
		assets::ContentHash Digest;
	};

	// What the server believes a client is holding, and the proof of it.
	//
	// @since v0.15
	struct GroupSignatures {
		// The tick the digests were taken at, and the only thing an answer may
		// name.
		uint64_t Tick = 0;

		// The components hashed, by name, in the order their ordinals count in.
		//
		// **Only the ones that produced a leaf**, which is what keeps this from
		// dominating the message: a slice of a few dozen entities touches a
		// fraction of what a host replicates, and naming the rest would spend
		// most of a datagram saying nothing.
		std::vector<core::Name> Components;

		// One entry per group in this audit's slice.
		std::vector<AuditGroup> Groups;
	};

	// A client saying which of an audit's groups it disagrees with.
	//
	// **Every field of this is hostile and the limit on it is the server's.**
	// A client claiming everything mismatches is asking the server to resend
	// the world, so nothing here is trusted: the tick has to be the audit the
	// server actually issued, the groups have to be ones it actually asked
	// about, and it may be answered once. See `Authority::Receive`.
	//
	// @since v0.15
	struct Disputed {
		// Which audit this answers.
		uint64_t Tick = 0;

		// The groups whose digests did not match, ascending.
		std::vector<uint32_t> Groups;
	};

	// The last tick a client applied in full.
	//
	// @since v0.3
	struct Applied {
		// The last tick the client applied in full.
		//
		// **In full is the load-bearing part.** A tick's delta may be several
		// messages, and acknowledging a partly-applied one would retire a
		// baseline the client does not actually hold.
		uint64_t Tick = 0;
	};

	// Writes an identity claim.
	//
	// @param writer   Where the bytes go.
	// @param identify The claim to write.
	// @since v0.9
	void WriteMessage(core::ByteWriter &writer, const Identify &identify);

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

	// Writes a message this module does not read.
	//
	// @param writer Where the bytes go.
	// @param user   The payload.
	// @since v0.13
	void WriteMessage(core::ByteWriter &writer, const User &user);

	// Writes an audit.
	//
	// @param writer     Where the bytes go.
	// @param signatures The groups and their digests.
	// @since v0.15
	void WriteMessage(core::ByteWriter &writer, const GroupSignatures &signatures);

	// Writes an answer to an audit.
	//
	// @param writer   Where the bytes go.
	// @param disputed The groups that did not match.
	// @since v0.15
	void WriteMessage(core::ByteWriter &writer, const Disputed &disputed);

	// What a successful read produced.
	//
	// @since v0.3
	struct Message {
		// Which of the payloads below was filled in.
		MessageKind Kind = MessageKind::Applied;

		// The payloads, one per kind.
		//
		// **Every field rather than a union, and it costs what it costs.** A
		// message is parsed once per datagram and then acted on, so the saving a
		// union would buy is a few hundred bytes on one stack frame — against a
		// visitor at every site that handles a message, in a module where every
		// inbound field is hostile and the parsing is the part that has to stay
		// readable.
		//
		// **Only the one `Kind` names has been written.** `ReadMessage` leaves
		// the rest at their defaults, so a reader that switches on something
		// other than `Kind` is reading a value no sender sent.
		//@{
		SnapshotChunk Chunk;
		replication::Delta Delta;
		replication::Structure Structure;
		replication::Input Input;
		replication::Applied Applied;
		replication::Identify Identify;
		replication::User User;
		replication::GroupSignatures Signatures;
		replication::Disputed Disputed;
		//@}
	};

	// Reads a message, refusing anything that is not one.
	//
	// @param reader  The bytes to parse.
	// @param message Filled in on success; untouched otherwise.
	// @return `false` on anything malformed. Drop it and count it.
	// @since v0.3
	bool ReadMessage(core::ByteReader &reader, Message &message);

	// What kind a message is, without parsing the rest of it.
	//
	// **`net::Packet::PeekChannel`'s shape, one layer up, and for the same
	// reason**: a router has to decide where a message goes before whoever
	// handles it has parsed it, and parsing twice to find out is the cost that
	// makes routing look expensive.
	//
	// @param message The bytes.
	// @return The kind, or nothing when the bytes are not a message this build
	//         understands.
	// @since v0.13
	std::optional<MessageKind> PeekMessageKind(std::span<const std::byte> message);

	// The most entities or components one message may carry.
	//
	// Bounds allocations caused by untrusted message counts.
	inline constexpr uint32_t MAXIMUM_ENTRIES = 1u << 20u;

	// The most parts one tick's delta may be split into.
	//
	// @since v0.5
	inline constexpr uint16_t MAXIMUM_PARTS = 1024;

	// The most groups one audit, or one answer to one, may name.
	//
	// Far below `MAXIMUM_ENTRIES` on purpose: an audit is a slice and an answer
	// is a subset of that slice, so a message naming hundreds of groups is a
	// message no honest sender produces. Bounding the parse is the cheap half
	// of `Authority::Receive`'s rate limit.
	//
	// @since v0.15
	inline constexpr uint32_t MAXIMUM_AUDIT_GROUPS = 256;
}
