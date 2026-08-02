#include <engine/ecs/Components.hpp>
#include <engine/world/Postbox.hpp>

#include <utility>

namespace engine::world {

	namespace {
		// The mailbox resources are created on first use rather than by the
		// world's constructor, so a world that never touches a bus carries
		// neither.
		Outbox &ReachOutbox(ecs::Store &store) {
			if (!store.HasResource<Outbox>()) {
				store.SetResource(Outbox{});
			}
			return *store.ResourceMutable<Outbox>();
		}

		BusBudget &ReachBudget(ecs::Store &store) {
			if (!store.HasResource<BusBudget>()) {
				store.SetResource(BusBudget{});
			}
			return *store.ResourceMutable<BusBudget>();
		}

		// Envelopes and deliveries hold a vector and a Name, so neither can be
		// written as its object representation: the vector is a pointer into
		// this process and the name is a process-local id.
		void WriteEnvelopes(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *boxes = static_cast<const Outbox *>(source);
			for (size_t index = 0; index < count; index++) {
				const Outbox &box = boxes[index];

				writer.WriteUInt64(box.NextTicket);
				writer.WriteUInt64(box.NextSequence);
				writer.WriteUInt32(static_cast<uint32_t>(box.Pending.size()));

				for (const Envelope &envelope : box.Pending) {
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
			}
		}

		void ReadEnvelopes(core::ByteReader &reader, void *destination, size_t count) {
			auto *boxes = static_cast<Outbox *>(destination);
			for (size_t index = 0; index < count; index++) {
				Outbox &box = boxes[index];
				box.Pending.clear();

				box.NextTicket = reader.ReadUInt64();
				box.NextSequence = reader.ReadUInt64();

				const uint32_t pending = reader.ReadUInt32();
				for (uint32_t at = 0; at < pending && !reader.Failed(); at++) {
					Envelope envelope;
					envelope.Bus = static_cast<BusKind>(reader.ReadUInt8());
					envelope.Operation = static_cast<BusOperation>(reader.ReadUInt8());
					envelope.Key = reader.ReadName();
					envelope.From = reader.ReadName();
					envelope.Sequence = reader.ReadUInt64();
					envelope.Reply.Value = reader.ReadUInt64();
					envelope.Version = reader.ReadUInt64();

					const uint32_t bytes = reader.ReadUInt32();
					envelope.Payload.resize(reader.Failed() ? 0 : bytes);
					if (!envelope.Payload.empty()) {
						reader.ReadRaw(envelope.Payload.data(), envelope.Payload.size());
					}

					box.Pending.push_back(std::move(envelope));
				}
			}
		}

		void WriteDeliveries(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *boxes = static_cast<const Inbox *>(source);
			for (size_t index = 0; index < count; index++) {
				const Inbox &box = boxes[index];
				writer.WriteUInt32(static_cast<uint32_t>(box.Arrived.size()));

				for (const Delivery &delivery : box.Arrived) {
					writer.WriteUInt8(static_cast<uint8_t>(delivery.Bus));
					writer.WriteName(delivery.Key);
					writer.WriteName(delivery.From);
					writer.WriteUInt64(delivery.Reply.Value);
					writer.WriteUInt8(static_cast<uint8_t>(delivery.Status));
					writer.WriteUInt64(delivery.Version);
					writer.WriteUInt32(static_cast<uint32_t>(delivery.Payload.size()));
					writer.WriteRaw(delivery.Payload.data(), delivery.Payload.size());
				}
			}
		}

		void ReadDeliveries(core::ByteReader &reader, void *destination, size_t count) {
			auto *boxes = static_cast<Inbox *>(destination);
			for (size_t index = 0; index < count; index++) {
				Inbox &box = boxes[index];
				box.Arrived.clear();

				const uint32_t arrived = reader.ReadUInt32();
				for (uint32_t at = 0; at < arrived && !reader.Failed(); at++) {
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

					box.Arrived.push_back(std::move(delivery));
				}
			}
		}
	}

	void RegisterMailboxTypes() {
		ecs::Components::Register<Outbox>("world.Outbox", WriteEnvelopes, ReadEnvelopes);
		ecs::Components::Register<Inbox>("world.Inbox", WriteDeliveries, ReadDeliveries);
		ecs::Components::Register<BusBudget>("world.BusBudget");
		ecs::Components::Register<Replica>("world.Replica");
	}

	std::span<const Delivery> Postbox::Deliveries() const {
		const Inbox *inbox = Store_->Resource<Inbox>();
		if (inbox == nullptr) {
			return {};
		}
		return {inbox->Arrived.data(), inbox->Arrived.size()};
	}

	bool Postbox::IsReplica() const {
		const Replica *replica = Store_->Resource<Replica>();
		return replica != nullptr && replica->Active;
	}

	uint32_t Postbox::Remaining() const {
		const BusBudget *budget = Store_->Resource<BusBudget>();
		if (budget == nullptr) {
			return BusBudget{}.PerTick;
		}
		return budget->Spent >= budget->PerTick ? 0 : budget->PerTick - budget->Spent;
	}

	Ticket Postbox::Post(
		BusKind bus,
		BusOperation operation,
		core::Name key,
		std::span<const std::byte> payload,
		uint64_t version,
		bool wantsReply
	) {
		if (IsReplica()) {
			// A replica simulates its own copy and reconciles against
			// authoritative state. Writing to a bus would be a client telling
			// the universe something the server never said — so it is refused
			// here, at the call, rather than left as a rule somebody
			// eventually breaks.
			return Ticket{};
		}

		BusBudget &budget = ReachBudget(*Store_);
		if (budget.Spent >= budget.PerTick) {
			// Refused here rather than at the barrier, so the world learns
			// immediately and can stop rather than queueing traffic that will
			// only be dropped.
			return Ticket{};
		}
		budget.Spent++;

		Outbox &outbox = ReachOutbox(*Store_);

		Envelope envelope;
		envelope.Bus = bus;
		envelope.Operation = operation;
		envelope.Key = key;
		envelope.Sequence = outbox.NextSequence++;
		envelope.Version = version;
		envelope.Payload.assign(payload.begin(), payload.end());

		if (wantsReply) {
			envelope.Reply = Ticket{outbox.NextTicket++};
		}

		const Ticket issued = envelope.Reply;
		outbox.Pending.push_back(std::move(envelope));

		// `From` is stamped by the driver at the barrier rather than here. A
		// world does not get to say who it is — that is the one field a
		// compromised or buggy world could otherwise lie about, and every
		// ordering decision depends on it.
		return issued;
	}

	// The three fire-and-forget operations check the budget themselves, because
	// `Post` reports failure by returning no ticket and these never ask for one
	// — so success and refusal would otherwise look identical.

	bool Postbox::Publish(std::string_view topic, std::span<const std::byte> payload) {
		if (IsReplica() || Remaining() == 0) {
			return false;
		}
		Post(BusKind::Messaging, BusOperation::Publish, core::Name(topic), payload, 0, false);
		return true;
	}

	bool Postbox::Subscribe(std::string_view topic) {
		if (IsReplica() || Remaining() == 0) {
			return false;
		}
		Post(BusKind::Messaging, BusOperation::Subscribe, core::Name(topic), {}, 0, false);
		return true;
	}

	bool Postbox::Unsubscribe(std::string_view topic) {
		if (IsReplica() || Remaining() == 0) {
			return false;
		}
		Post(BusKind::Messaging, BusOperation::Unsubscribe, core::Name(topic), {}, 0, false);
		return true;
	}

	Ticket Postbox::Get(BusKind bus, std::string_view key) {
		return Post(bus, BusOperation::Get, core::Name(key), {}, 0, true);
	}

	Ticket Postbox::Set(BusKind bus, std::string_view key, std::span<const std::byte> payload) {
		return Post(bus, BusOperation::Set, core::Name(key), payload, 0, true);
	}

	Ticket Postbox::Update(std::string_view key, uint64_t version, std::span<const std::byte> payload) {
		return Post(BusKind::DataStore, BusOperation::Update, core::Name(key), payload, version, true);
	}

	Ticket Postbox::Remove(BusKind bus, std::string_view key) {
		return Post(bus, BusOperation::Remove, core::Name(key), {}, 0, true);
	}

	Ticket Postbox::Push(std::string_view key, std::span<const std::byte> payload) {
		return Post(BusKind::MemoryStore, BusOperation::Push, core::Name(key), payload, 0, true);
	}

	Ticket Postbox::Pop(std::string_view key) {
		return Post(BusKind::MemoryStore, BusOperation::Pop, core::Name(key), {}, 0, true);
	}

	Ticket Postbox::Teleport(std::string_view world, std::span<const std::byte> payload) {
		return Post(BusKind::Teleport, BusOperation::Send, core::Name(world), payload, 0, true);
	}
}
