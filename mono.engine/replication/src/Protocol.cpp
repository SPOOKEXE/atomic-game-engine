#include <engine/replication/Protocol.hpp>

#include <utility>

namespace engine::replication {

	namespace {
		// Written first on every message, so a reader that has been handed
		// something else fails on its first field rather than interpreting
		// arbitrary bytes as a count.
		void WriteFront(core::ByteWriter &writer, MessageKind kind) {
			writer.WriteUInt16(PROTOCOL_VERSION);
			writer.WriteUInt8(static_cast<uint8_t>(kind));
		}

		void WriteBytes(core::ByteWriter &writer, std::span<const std::byte> bytes) {
			writer.WriteUInt32(static_cast<uint32_t>(bytes.size()));
			writer.WriteRaw(bytes.data(), bytes.size());
		}

		// Sized from the reader's state rather than from the claimed length: a
		// truncated stream can claim any size, and resizing to a claim is how a
		// corrupt message becomes an allocation failure.
		bool ReadBytes(core::ByteReader &reader, std::vector<std::byte> &into) {
			const uint32_t count = reader.ReadUInt32();
			if (reader.Failed() || count > reader.Remaining()) {
				return false;
			}

			into.resize(count);
			if (count > 0) {
				reader.ReadRaw(into.data(), into.size());
			}
			return !reader.Failed();
		}

		void WriteEntities(core::ByteWriter &writer, const std::vector<ecs::Entity> &entities) {
			writer.WriteUInt32(static_cast<uint32_t>(entities.size()));
			for (const ecs::Entity entity : entities) {
				writer.WriteUInt64(entity.Id);
			}
		}

		bool ReadEntities(core::ByteReader &reader, std::vector<ecs::Entity> &into) {
			const uint32_t count = reader.ReadUInt32();
			if (reader.Failed() || count > MAXIMUM_ENTRIES) {
				return false;
			}

			// Eight bytes each, so a count claiming more than the buffer holds
			// is refused before anything is reserved for it.
			if (static_cast<size_t>(count) * sizeof(uint64_t) > reader.Remaining()) {
				return false;
			}

			into.resize(count);
			for (uint32_t index = 0; index < count; index++) {
				into[index] = ecs::Entity{reader.ReadUInt64()};
			}
			return !reader.Failed();
		}
	}

	const char *Describe(MessageKind kind) {
		switch (kind) {
		case MessageKind::SnapshotChunk:
			return "snapshot chunk";
		case MessageKind::Delta:
			return "delta";
		case MessageKind::Structure:
			return "structure";
		case MessageKind::Input:
			return "input";
		case MessageKind::Applied:
			return "applied";
		case MessageKind::Identify:
			return "identify";
		}
		// No default label, so adding a kind is a compiler warning here.
		return "?";
	}

	void WriteMessage(core::ByteWriter &writer, const Identify &identify) {
		WriteFront(writer, MessageKind::Identify);
		writer.WriteRaw(identify.Key.Value.data(), identify.Key.Value.size());
		writer.WriteRaw(identify.Signature.Value.data(), identify.Signature.Value.size());
	}

	void WriteMessage(core::ByteWriter &writer, const SnapshotChunk &chunk) {
		WriteFront(writer, MessageKind::SnapshotChunk);
		writer.WriteUInt64(chunk.Tick);
		writer.WriteUInt32(chunk.TotalBytes);
		writer.WriteUInt32(chunk.Offset);
		WriteBytes(writer, chunk.Bytes);
	}

	void WriteMessage(core::ByteWriter &writer, const Delta &delta) {
		WriteFront(writer, MessageKind::Delta);
		writer.WriteUInt64(delta.Tick);
		writer.WriteUInt64(delta.Baseline);

		// Three bytes on every part of every tick, which is what it costs to
		// let a receiver tell "that is all of tick N" from "that is all of tick
		// N that arrived". Fixed width, so re-encoding a part to set the marker
		// cannot change the size the packing already budgeted for.
		writer.WriteUInt16(delta.Part);
		writer.WriteBool(delta.Final);

		writer.WriteUInt32(static_cast<uint32_t>(delta.Components.size()));
		for (const ComponentDelta &component : delta.Components) {
			// By name. An id is a dense counter assigned in registration order
			// and means something else in the process reading this.
			writer.WriteName(component.Component);
			WriteEntities(writer, component.Entities);
			WriteBytes(writer, component.Values);
		}
	}

	void WriteMessage(core::ByteWriter &writer, const Structure &structure) {
		WriteFront(writer, MessageKind::Structure);
		writer.WriteUInt64(structure.Tick);
		WriteEntities(writer, structure.Created);
		WriteEntities(writer, structure.Destroyed);
		WriteEntities(writer, structure.Forgotten);
	}

	void WriteMessage(core::ByteWriter &writer, const Input &input) {
		WriteFront(writer, MessageKind::Input);
		writer.WriteUInt64(input.Tick);
		WriteBytes(writer, input.Bytes);
	}

	void WriteMessage(core::ByteWriter &writer, const Applied &applied) {
		WriteFront(writer, MessageKind::Applied);
		writer.WriteUInt64(applied.Tick);
	}

	bool ReadMessage(core::ByteReader &reader, Message &message) {
		if (reader.ReadUInt16() != PROTOCOL_VERSION) {
			return false;
		}

		const uint8_t kind = reader.ReadUInt8();
		// **The last kind, and it has to be updated when one is appended.** This
		// is the one place adding a `MessageKind` fails silently: the switches
		// below are `-Wswitch`-checked and this comparison is not, so a new kind
		// parses as out of range and every message of it is dropped as
		// malformed. `Identify` did exactly that.
		if (reader.Failed() || kind > static_cast<uint8_t>(MessageKind::Identify)) {
			// Range-checked before the cast. Casting anyway produces a value no
			// switch handles, and every `Describe` and dispatch downstream then
			// reads something the type says cannot exist.
			return false;
		}

		// Built here and assigned at the end, so a message that failed part-way
		// leaves the caller's object as it was.
		Message read;
		read.Kind = static_cast<MessageKind>(kind);

		switch (read.Kind) {
		case MessageKind::SnapshotChunk: {
			read.Chunk.Tick = reader.ReadUInt64();
			read.Chunk.TotalBytes = reader.ReadUInt32();
			read.Chunk.Offset = reader.ReadUInt32();
			if (reader.Failed() || !ReadBytes(reader, read.Chunk.Bytes)) {
				return false;
			}

			// A chunk that runs past the total it declares is either a bug or
			// somebody probing the reassembly buffer.
			const uint64_t end = static_cast<uint64_t>(read.Chunk.Offset) + read.Chunk.Bytes.size();
			if (end > read.Chunk.TotalBytes) {
				return false;
			}
			break;
		}

		case MessageKind::Delta: {
			read.Delta.Tick = reader.ReadUInt64();
			read.Delta.Baseline = reader.ReadUInt64();
			read.Delta.Part = reader.ReadUInt16();
			read.Delta.Final = reader.ReadBool();

			// Bounded before anything is remembered about it. A receiver keeps
			// one bit per part of the tick it is counting, so an unbounded part
			// number is a peer deciding how large that record is.
			if (reader.Failed() || read.Delta.Part >= MAXIMUM_PARTS) {
				return false;
			}

			const uint32_t components = reader.ReadUInt32();
			if (reader.Failed() || components > MAXIMUM_ENTRIES) {
				return false;
			}

			read.Delta.Components.reserve(components);
			for (uint32_t index = 0; index < components; index++) {
				ComponentDelta component;
				component.Component = reader.ReadName();
				if (reader.Failed() || !component.Component.IsValid()) {
					return false;
				}
				if (!ReadEntities(reader, component.Entities) || !ReadBytes(reader, component.Values)) {
					return false;
				}
				read.Delta.Components.push_back(std::move(component));
			}
			break;
		}

		case MessageKind::Structure:
			read.Structure.Tick = reader.ReadUInt64();
			if (reader.Failed() || !ReadEntities(reader, read.Structure.Created) ||
				!ReadEntities(reader, read.Structure.Destroyed) ||
				!ReadEntities(reader, read.Structure.Forgotten)) {
				return false;
			}
			break;

		case MessageKind::Input:
			read.Input.Tick = reader.ReadUInt64();
			if (reader.Failed() || !ReadBytes(reader, read.Input.Bytes)) {
				return false;
			}
			break;

		case MessageKind::Applied:
			read.Applied.Tick = reader.ReadUInt64();
			break;

		case MessageKind::Identify:
			// Fixed length both, so there is no count a sender chooses and
			// nothing to bound.
			if (!reader.ReadRaw(read.Identify.Key.Value.data(), read.Identify.Key.Value.size()) ||
				!reader.ReadRaw(read.Identify.Signature.Value.data(), read.Identify.Signature.Value.size())) {
				return false;
			}
			break;
		}

		if (reader.Failed()) {
			return false;
		}

		message = std::move(read);
		return true;
	}
}
