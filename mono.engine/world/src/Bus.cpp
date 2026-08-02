#include <engine/world/Bus.hpp>

namespace engine::world {

	const char *Describe(BusKind bus) {
		switch (bus) {
		case BusKind::Messaging:
			return "MessagingService";
		case BusKind::MemoryStore:
			return "MemoryStore";
		case BusKind::DataStore:
			return "DataStore";
		case BusKind::Teleport:
			return "Teleport";
		}
		return "?";
	}

	const char *Describe(BusOperation operation) {
		switch (operation) {
		case BusOperation::Publish:
			return "publish";
		case BusOperation::Subscribe:
			return "subscribe";
		case BusOperation::Unsubscribe:
			return "unsubscribe";
		case BusOperation::Get:
			return "get";
		case BusOperation::Set:
			return "set";
		case BusOperation::Remove:
			return "remove";
		case BusOperation::Push:
			return "push";
		case BusOperation::Pop:
			return "pop";
		case BusOperation::Update:
			return "update";
		case BusOperation::Send:
			return "send";
		}
		return "?";
	}

	void WriteEnvelope(core::ByteWriter &writer, const Envelope &envelope) {
		writer.WriteUInt8(static_cast<uint8_t>(envelope.Bus));
		writer.WriteUInt8(static_cast<uint8_t>(envelope.Operation));
		writer.WriteName(envelope.Key);
		writer.WriteName(envelope.From);
		writer.WriteUInt64(envelope.Sequence);
		writer.WriteUInt64(envelope.Reply.Value);
		writer.WriteUInt64(envelope.Version);
		writer.WriteUInt32(static_cast<uint32_t>(envelope.Payload.size()));
		writer.WriteRaw(envelope.Payload.data(), envelope.Payload.size());
	}

	Envelope ReadEnvelope(core::ByteReader &reader) {
		Envelope envelope;
		envelope.Bus = static_cast<BusKind>(reader.ReadUInt8());
		envelope.Operation = static_cast<BusOperation>(reader.ReadUInt8());
		envelope.Key = reader.ReadName();
		envelope.From = reader.ReadName();
		envelope.Sequence = reader.ReadUInt64();
		envelope.Reply.Value = reader.ReadUInt64();
		envelope.Version = reader.ReadUInt64();

		const uint32_t bytes = reader.ReadUInt32();
		// Sized from the reader's state rather than the header: a truncated
		// stream can claim any length, and resizing to a claim is how a corrupt
		// frame becomes an allocation failure.
		envelope.Payload.resize(reader.Failed() ? 0 : bytes);
		if (!envelope.Payload.empty()) {
			reader.ReadRaw(envelope.Payload.data(), envelope.Payload.size());
		}
		return envelope;
	}

	void WriteDelivery(core::ByteWriter &writer, const Delivery &delivery) {
		writer.WriteUInt8(static_cast<uint8_t>(delivery.Bus));
		writer.WriteName(delivery.Key);
		writer.WriteName(delivery.From);
		writer.WriteUInt64(delivery.Reply.Value);
		writer.WriteUInt8(static_cast<uint8_t>(delivery.Status));
		writer.WriteUInt64(delivery.Version);
		writer.WriteUInt32(static_cast<uint32_t>(delivery.Payload.size()));
		writer.WriteRaw(delivery.Payload.data(), delivery.Payload.size());
	}

	Delivery ReadDelivery(core::ByteReader &reader) {
		Delivery delivery;
		delivery.Bus = static_cast<BusKind>(reader.ReadUInt8());
		delivery.Key = reader.ReadName();
		delivery.From = reader.ReadName();
		delivery.Reply.Value = reader.ReadUInt64();
		delivery.Status = static_cast<BusStatus>(reader.ReadUInt8());
		delivery.Version = reader.ReadUInt64();

		const uint32_t bytes = reader.ReadUInt32();
		delivery.Payload.resize(reader.Failed() ? 0 : bytes);
		if (!delivery.Payload.empty()) {
			reader.ReadRaw(delivery.Payload.data(), delivery.Payload.size());
		}
		return delivery;
	}

	const char *Describe(BusStatus status) {
		switch (status) {
		case BusStatus::Ok:
			return "ok";
		case BusStatus::NotFound:
			return "not found";
		case BusStatus::Conflict:
			return "version conflict";
		case BusStatus::OverBudget:
			return "over budget";
		case BusStatus::NoSuchWorld:
			return "no such world";
		case BusStatus::Unsupported:
			return "unsupported operation";
		}
		return "?";
	}
}
