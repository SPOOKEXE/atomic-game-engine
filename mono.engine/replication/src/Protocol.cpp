#include <engine/replication/Protocol.hpp>

#include <utility>

namespace engine::replication {

	namespace {
		void WriteFront(core::ByteWriter &writer, MessageKind kind) {
			writer.WriteUInt16(PROTOCOL_VERSION);
			writer.WriteUInt8(static_cast<uint8_t>(kind));
		}

		void WriteBytes(core::ByteWriter &writer, std::span<const std::byte> bytes) {
			writer.WriteUInt32(static_cast<uint32_t>(bytes.size()));
			writer.WriteRaw(bytes.data(), bytes.size());
		}

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
		case MessageKind::User:
			return "user";
		case MessageKind::GroupSignatures:
			return "group signatures";
		case MessageKind::Disputed:
			return "disputed";
		}
		return "?";
	}

	void WriteMessage(core::ByteWriter &writer, const Identify &identify) {
		WriteFront(writer, MessageKind::Identify);
		writer.WriteRaw(identify.Key.Value.data(), identify.Key.Value.size());
		writer.WriteRaw(identify.Signature.Value.data(), identify.Signature.Value.size());
	}

	void WriteMessage(core::ByteWriter &writer, const SnapshotChunk &chunk) {
		WriteFront(writer, MessageKind::SnapshotChunk);
		writer.WriteUInt8(static_cast<uint8_t>(chunk.Stage));
		writer.WriteUInt64(chunk.Tick);
		writer.WriteUInt32(chunk.TotalBytes);
		writer.WriteUInt32(chunk.Offset);
		WriteBytes(writer, chunk.Bytes);
	}

	void WriteMessage(core::ByteWriter &writer, const Delta &delta) {
		WriteFront(writer, MessageKind::Delta);
		writer.WriteUInt64(delta.Tick);
		writer.WriteUInt64(delta.Baseline);

		writer.WriteUInt16(delta.Part);
		writer.WriteBool(delta.Final);

		writer.WriteUInt32(static_cast<uint32_t>(delta.Components.size()));
		for (const ComponentDelta &component : delta.Components) {
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

	std::optional<MessageKind> PeekMessageKind(std::span<const std::byte> message) {
		core::ByteReader reader(message);
		const uint16_t version = reader.ReadUInt16();
		const uint8_t kind = reader.ReadUInt8();
		if (reader.Failed() || version != PROTOCOL_VERSION ||
			kind > static_cast<uint8_t>(MessageKind::Disputed)) {
			return std::nullopt;
		}
		return static_cast<MessageKind>(kind);
	}

	void WriteMessage(core::ByteWriter &writer, const User &user) {
		WriteFront(writer, MessageKind::User);
		WriteBytes(writer, user.Bytes);
	}

	void WriteMessage(core::ByteWriter &writer, const GroupSignatures &signatures) {
		WriteFront(writer, MessageKind::GroupSignatures);
		writer.WriteUInt64(signatures.Tick);

		writer.WriteUInt32(static_cast<uint32_t>(signatures.Components.size()));
		for (const core::Name component : signatures.Components) {
			writer.WriteName(component);
		}

		writer.WriteUInt32(static_cast<uint32_t>(signatures.Groups.size()));
		for (const AuditGroup &group : signatures.Groups) {
			writer.WriteUInt32(group.Group);
			WriteEntities(writer, group.Entities);
			writer.WriteRaw(group.Digest.Digest.data(), group.Digest.Digest.size());
		}
	}

	void WriteMessage(core::ByteWriter &writer, const Disputed &disputed) {
		WriteFront(writer, MessageKind::Disputed);
		writer.WriteUInt64(disputed.Tick);
		writer.WriteUInt32(static_cast<uint32_t>(disputed.Groups.size()));
		for (const uint32_t group : disputed.Groups) {
			writer.WriteUInt32(group);
		}
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
		if (reader.Failed() || kind > static_cast<uint8_t>(MessageKind::Disputed)) {
			return false;
		}

		Message read;
		read.Kind = static_cast<MessageKind>(kind);

		switch (read.Kind) {
		case MessageKind::SnapshotChunk: {
			// Range-checked here rather than cast blindly, for the reason this
			// module's `AGENTS.md` gives about `MessageKind`: a value outside
			// the enum reaches a `-Wswitch`-checked switch that has no arm for
			// it, and the compiler cannot warn about a number it never saw.
			const uint8_t stage = reader.ReadUInt8();
			if (reader.Failed() || stage > static_cast<uint8_t>(SnapshotStage::World)) {
				return false;
			}
			read.Chunk.Stage = static_cast<SnapshotStage>(stage);

			read.Chunk.Tick = reader.ReadUInt64();
			read.Chunk.TotalBytes = reader.ReadUInt32();
			read.Chunk.Offset = reader.ReadUInt32();
			if (reader.Failed() || !ReadBytes(reader, read.Chunk.Bytes)) {
				return false;
			}

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
			if (!reader.ReadRaw(read.Identify.Key.Value.data(), read.Identify.Key.Value.size()) ||
				!reader.ReadRaw(read.Identify.Signature.Value.data(), read.Identify.Signature.Value.size())) {
				return false;
			}
			break;

		case MessageKind::User:
			if (!ReadBytes(reader, read.User.Bytes)) {
				return false;
			}
			break;

		case MessageKind::GroupSignatures: {
			read.Signatures.Tick = reader.ReadUInt64();

			const uint32_t components = reader.ReadUInt32();
			if (reader.Failed() || components > MAXIMUM_ENTRIES) {
				return false;
			}

			read.Signatures.Components.reserve(components);
			for (uint32_t index = 0; index < components; index++) {
				const core::Name component = reader.ReadName();
				if (reader.Failed() || !component.IsValid()) {
					return false;
				}
				read.Signatures.Components.push_back(component);
			}

			const uint32_t groups = reader.ReadUInt32();
			if (reader.Failed() || groups > MAXIMUM_AUDIT_GROUPS) {
				return false;
			}

			read.Signatures.Groups.reserve(groups);
			for (uint32_t index = 0; index < groups; index++) {
				AuditGroup group;
				group.Group = reader.ReadUInt32();
				if (reader.Failed() || !ReadEntities(reader, group.Entities)) {
					return false;
				}
				if (!reader.ReadRaw(group.Digest.Digest.data(), group.Digest.Digest.size())) {
					return false;
				}
				read.Signatures.Groups.push_back(std::move(group));
			}
			break;
		}

		case MessageKind::Disputed: {
			read.Disputed.Tick = reader.ReadUInt64();

			const uint32_t groups = reader.ReadUInt32();
			if (reader.Failed() || groups > MAXIMUM_AUDIT_GROUPS) {
				return false;
			}

			read.Disputed.Groups.resize(groups);
			for (uint32_t index = 0; index < groups; index++) {
				read.Disputed.Groups[index] = reader.ReadUInt32();
			}
			if (reader.Failed()) {
				return false;
			}
			break;
		}
		}

		// A replication packet carries exactly one message. Leaving a suffix for a
		// hypothetical second parser would give the same bytes two meanings.
		if (reader.Failed() || !reader.AtEnd()) {
			return false;
		}

		message = std::move(read);
		return true;
	}
}
