#pragma once

// @tier L12 · shared

#include <engine/assets/Signature.hpp>
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
		SnapshotChunk,

		Delta,

		Structure,

		Input,

		Applied,

		// @since v0.9
		Identify,
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
		assets::PublicKey Key;

		assets::SignatureBytes Signature;
	};

	// The wire format version.
	//
	// Unknown versions are refused; wire changes require a version bump.
	inline constexpr uint16_t PROTOCOL_VERSION = 5;

	// One piece of a snapshot.
	//
	// @since v0.3
	struct SnapshotChunk {
		uint64_t Tick = 0;

		uint32_t TotalBytes = 0;

		uint32_t Offset = 0;

		std::vector<std::byte> Bytes;
	};

	// One component's changed rows, for one tick.
	//
	// @since v0.3
	struct ComponentDelta {
		core::Name Component;

		std::vector<ecs::Entity> Entities;

		std::vector<std::byte> Values;
	};

	// What moved in one tick.
	//
	// @since v0.3
	struct Delta {
		uint64_t Tick = 0;

		uint64_t Baseline = 0;

		// @since v0.5
		uint16_t Part = 0;

		// @since v0.5
		bool Final = true;

		std::vector<ComponentDelta> Components;
	};

	// Which entities a client holds, and the three ways that changes.
	//
	// @since v0.5
	struct Structure {
		uint64_t Tick = 0;

		std::vector<ecs::Entity> Created;

		std::vector<ecs::Entity> Destroyed;

		std::vector<ecs::Entity> Forgotten;
	};

	// What a player did in one tick.
	//
	// @since v0.3
	struct Input {
		uint64_t Tick = 0;

		std::vector<std::byte> Bytes;
	};

	// The last tick a client applied in full.
	//
	// @since v0.3
	struct Applied {
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

	// What a successful read produced.
	//
	// @since v0.3
	struct Message {
		MessageKind Kind = MessageKind::Applied;

		SnapshotChunk Chunk;

		replication::Delta Delta;

		replication::Structure Structure;

		replication::Input Input;

		replication::Applied Applied;

		replication::Identify Identify;
	};

	// Reads a message, refusing anything that is not one.
	//
	// @param reader  The bytes to parse.
	// @param message Filled in on success; untouched otherwise.
	// @return `false` on anything malformed. Drop it and count it.
	// @since v0.3
	bool ReadMessage(core::ByteReader &reader, Message &message);

	// The most entities or components one message may carry.
	//
	// Bounds allocations caused by untrusted message counts.
	inline constexpr uint32_t MAXIMUM_ENTRIES = 1u << 20u;

	// The most parts one tick's delta may be split into.
	//
	// @since v0.5
	inline constexpr uint16_t MAXIMUM_PARTS = 1024;
}
