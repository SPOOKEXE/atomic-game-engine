#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Codec.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/script/TeleportRequest.hpp>
#include <engine/world/Postbox.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace engine::script {

	namespace {
		namespace scene = engine::scene;
		using world::Postbox;
		using world::Ticket;
		constexpr size_t MAXIMUM_PENDING_TELEPORT_REQUESTS = 16;
		constexpr size_t MAXIMUM_TELEPORT_REQUEST_DATA_BYTES = 64u * 1024u;
		constexpr uint64_t MAXIMUM_SCRIPT_TELEPORT_REQUEST_ID = (uint64_t{1} << 53) - 1;

		// A callback is meaningful only in the VM that retained it, and an outbox
		// only in the live connection that will send it. Both resources must be
		// snapshot-safe without letting a restored world revive stale local work.
		void WriteTransientTeleportState(core::ByteWriter &, const void *, size_t) {}

		void ReadTeleportRequestHandlers(core::ByteReader &, void *destination, size_t count) {
			auto *handlers = static_cast<TeleportRequestHandler *>(destination);
			for (size_t index = 0; index < count; index++) {
				handlers[index] = {};
			}
		}

		void ReadTeleportRequestOutboxes(core::ByteReader &, void *destination, size_t count) {
			auto *outboxes = static_cast<TeleportRequestOutbox *>(destination);
			for (size_t index = 0; index < count; index++) {
				outboxes[index] = {};
			}
		}

		HostValue HostValueOf(const ScriptValue &value) {
			switch (value.Tag) {
			case ValueTag::Nil:
				return {};
			case ValueTag::False:
				return HostValue::Of(false);
			case ValueTag::True:
				return HostValue::Of(true);
			case ValueTag::Number:
				return HostValue::Of(value.Number);
			case ValueTag::String:
				return HostValue::Of(value.Text);
			case ValueTag::Vector3: {
				HostValue result(HostTag::Vector3);
				result.Vector = value.Vector;
				return result;
			}
			case ValueTag::Color3: {
				HostValue result(HostTag::Color3);
				result.Colour = value.Colour;
				return result;
			}
			case ValueTag::CFrame: {
				HostValue result(HostTag::CFrame);
				result.Frame = value.Frame;
				return result;
			}
			case ValueTag::Array: {
				HostValue result(HostTag::Array);
				result.Items.reserve(value.Items.size());
				for (const ScriptValue &item : value.Items) {
					result.Items.push_back(HostValueOf(item));
				}
				return result;
			}
			case ValueTag::Map: {
				HostValue result(HostTag::Map);
				result.Entries.reserve(value.Entries.size());
				for (const auto &[name, item] : value.Entries) {
					result.Entries.emplace_back(name, HostValueOf(item));
				}
				return result;
			}
			}
			return {};
		}

		TeleportRequestResult Invalid(std::string message) {
			TeleportRequestResult result;
			result.Message = std::move(message);
			return result;
		}
	}

	std::string_view TeleportRequestDecisionName(TeleportRequestDecision decision) {
		switch (decision) {
		case TeleportRequestDecision::NotProcessed:
			return "NotProcessed";
		case TeleportRequestDecision::Denied:
			return "Denied";
		case TeleportRequestDecision::Processed:
			return "Processed";
		}
		return "NotProcessed";
	}

	void RegisterTeleportRequestComponents() {
		ecs::Components::Register<TeleportRequestHandler>(
			"script.TeleportRequestHandler", WriteTransientTeleportState, ReadTeleportRequestHandlers
		);
		ecs::Components::Register<TeleportRequestOutbox>(
			"script.TeleportRequestOutbox", WriteTransientTeleportState, ReadTeleportRequestOutboxes
		);
		ecs::EnumTable::Register(
			"TeleportRequestDecision", std::array<std::string_view, 3>{"NotProcessed", "Denied", "Processed"}
		);
	}

	bool QueueTeleportRequest(
		ecs::Store &store, std::string_view place, const ScriptValue &data, std::string &failure
	) {
		if (place.empty() || place.size() > 256) {
			failure = "Teleport: destination must contain at most 256 bytes";
			return false;
		}

		ScriptValue encoded = data;
		std::vector<std::byte> bytes;
		if (const CodecStatus status = Encode(encoded, bytes); status != CodecStatus::Ok) {
			failure = std::string("Teleport: the data cannot cross a world boundary: ") + Describe(status);
			return false;
		}
		if (bytes.size() > MAXIMUM_TELEPORT_REQUEST_DATA_BYTES) {
			failure = "Teleport: the data exceeds the 65536 byte request limit";
			return false;
		}

		if (!store.HasResource<TeleportRequestOutbox>()) {
			store.SetResource(TeleportRequestOutbox{});
		}
		TeleportRequestOutbox &outbox = *store.ResourceMutable<TeleportRequestOutbox>();
		if (outbox.Pending.size() + outbox.AwaitingResults.size() >= MAXIMUM_PENDING_TELEPORT_REQUESTS) {
			failure = "Teleport: too many requests are waiting for the server";
			return false;
		}

		const uint64_t id = outbox.NextId;
		outbox.NextId = outbox.NextId == MAXIMUM_SCRIPT_TELEPORT_REQUEST_ID ? 1 : outbox.NextId + 1;
		outbox.Pending.push_back({id, std::string(place), std::move(bytes)});
		return true;
	}

	std::span<const PendingTeleportRequest> PendingTeleportRequests(const ecs::Store &store) {
		const auto *outbox = store.Resource<TeleportRequestOutbox>();
		return outbox == nullptr ? std::span<const PendingTeleportRequest>{} : std::span(outbox->Pending);
	}

	void MarkTeleportRequestSent(ecs::Store &store) {
		auto *outbox = store.ResourceMutable<TeleportRequestOutbox>();
		if (outbox == nullptr || outbox->Pending.empty()) {
			return;
		}

		outbox->AwaitingResults.push_back(outbox->Pending.front().Id);
		outbox->Pending.erase(outbox->Pending.begin());
	}

	bool AcceptTeleportResult(ecs::Store &store, uint64_t id) {
		auto *outbox = store.ResourceMutable<TeleportRequestOutbox>();
		if (outbox == nullptr || id == 0) {
			return false;
		}

		const auto found = std::find(outbox->AwaitingResults.begin(), outbox->AwaitingResults.end(), id);
		if (found == outbox->AwaitingResults.end()) {
			return false;
		}
		outbox->AwaitingResults.erase(found);
		return true;
	}

	bool TeleportPlayer(
		ecs::Store &store,
		std::string_view place,
		ecs::Entity player,
		const ScriptValue *data,
		std::string &failure
	) {
		if (!store.Alive(player) || !store.IsA(player, scene::PlayerClass())) {
			failure = "Teleport: the player must be a Player";
			return false;
		}

		Postbox box(store);
		if (box.IsReplica() || store.AdoptOnly()) {
			failure = "Teleport: this world is a replica and does not decide who is in it";
			return false;
		}

		ScriptValue label{ValueTag::String};
		const core::Name name = store.InstanceNameOf(player);
		label.Text = name.IsValid() ? std::string(name.Text()) : std::string("Player");

		ScriptValue envelope{ValueTag::Map};
		envelope.Entries.emplace_back("Player", std::move(label));
		if (data != nullptr) {
			envelope.Entries.emplace_back("Data", *data);
		}

		std::vector<std::byte> payload;
		if (const CodecStatus status = Encode(envelope, payload); status != CodecStatus::Ok) {
			failure = std::string("Teleport: the data cannot cross a world boundary: ") + Describe(status);
			return false;
		}
		const std::string destination(place);
		if (box.Teleport(destination.c_str(), payload).Value == Ticket::NONE) {
			failure = std::string("Teleport: over this world's budget for '") + std::string(place) + "'";
			return false;
		}

		(void)scene::RemoveCharacter(store, player);
		store.DestroyInstance(player);
		return true;
	}

	TeleportRequestResult
	DispatchTeleportRequest(Runtime &runtime, ecs::Store &store, const TeleportRequest &request) {
		const auto *handler = store.Resource<TeleportRequestHandler>();
		if (handler == nullptr || !handler->Callback.Valid()) {
			return Invalid("TeleportService.TeleportRequested has no handler");
		}

		HostValue argument(HostTag::Map);
		argument.Entries.emplace_back("Player", HostValue::Of(request.Player));
		argument.Entries.emplace_back("Place", HostValue::Of(request.Place));
		argument.Entries.emplace_back("Data", HostValueOf(request.Data));
		const HostValue arguments[] = {std::move(argument)};

		HostValue returned;
		if (!runtime.Invoke(handler->Callback, arguments, returned)) {
			return Invalid("TeleportService.TeleportRequested failed");
		}
		if (returned.Tag != HostTag::Map) {
			return Invalid(
				"TeleportService.TeleportRequested must return { Decision = Enum.TeleportRequestDecision.*, "
				"Message? = string }"
			);
		}

		TeleportRequestResult result;
		bool hasDecision = false;
		for (const auto &[name, value] : returned.Entries) {
			if (name == "Decision" && value.Tag == HostTag::String) {
				hasDecision = true;
				if (value.Text == "NotProcessed") {
					result.Decision = TeleportRequestDecision::NotProcessed;
				} else if (value.Text == "Denied") {
					result.Decision = TeleportRequestDecision::Denied;
				} else if (value.Text == "Processed") {
					result.Decision = TeleportRequestDecision::Processed;
				} else {
					return Invalid("TeleportService.TeleportRequested returned an unknown decision");
				}
			} else if (name == "Message") {
				if (value.Tag != HostTag::String) {
					return Invalid("TeleportService.TeleportRequested Message must be a string");
				}
				result.Message = value.Text;
			}
		}
		return hasDecision ? result : Invalid("TeleportService.TeleportRequested returned no Decision");
	}
}
