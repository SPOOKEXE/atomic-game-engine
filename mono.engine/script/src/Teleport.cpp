// The admission pass, which is a system and not a binding.
//
// `Teleport.hpp` carries why this is not in `BusServices.cpp`; `Runtime.hpp`
// carries why it runs on every world.
//
// @tier L9 · shared
// @since v0.18

#include "Teleport.hpp"

#include "Codec.hpp"

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/world/Postbox.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace engine::script {

	namespace {
		using world::BusKind;
		using world::Delivery;

		// Rebuilds an arriving player in this world.
		//
		// **The engine admits them, not a script.** Who is in a game is the
		// host's business — `scene::AddPlayer` says so — and a teleport that
		// only worked in games whose author had written an arrival handler
		// would be a feature with a footnote. `Players.PlayerAdded` fires from
		// the parenting, so a game that *wants* to react already can.
		void AdmitArrival(ecs::Store &store, const Delivery &delivery) {
			ScriptValue envelope;
			if (Decode(delivery.Payload, envelope) != CodecStatus::Ok || envelope.Tag != ValueTag::Map) {
				return;
			}

			std::string name = "Player";
			const ScriptValue *data = nullptr;
			for (const auto &entry : envelope.Entries) {
				if (entry.first == "Player" && entry.second.Tag == ValueTag::String) {
					name = entry.second.Text;
				} else if (entry.first == "Data") {
					data = &entry.second;
				}
			}

			const ecs::Entity player = scene::AddPlayer(store, name);
			if (player == ecs::NULL_ENTITY) {
				// A world with no `Players` service takes nobody. Quiet rather
				// than an error, for `mono.server`'s reason: that is the
				// placeholder scene and it is furnished by nobody.
				return;
			}

			(void)scene::LoadCharacter(store, player);

			if (data == nullptr) {
				return;
			}

			// A copy, because `Encode` sorts a map's entries in place and the
			// delivery's tree is not this function's to reorder.
			ScriptValue carried = *data;

			std::vector<std::byte> bytes;
			if (Encode(carried, bytes) != CodecStatus::Ok) {
				return;
			}

			const ecs::Entity held =
				store.CreateInstance(ecs::Classes::Find(core::Name("StringValue")), TELEPORT_DATA);
			if (held == ecs::NULL_ENTITY) {
				return;
			}

			scene::TextContent text;
			text.Value.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
			store.Set(held, text);
			store.SetParent(held, player);
		}
	}

	size_t AdmitTeleports(ecs::Store &store) {
		size_t admitted = 0;

		// **Taken out of the inbox as it is admitted, and that is not tidiness.**
		// Reading without consuming means *whoever asks twice gets two people*,
		// and a host installing this system twice is not a hypothetical: the
		// studio's `Editor::BuildWorld` calls `client::InstallPresentation`,
		// which registers it, and then registered it again beside the physics
		// and gravity systems. Every arrival was therefore admitted twice — two
		// `Player` rows, two characters, one of them adopted by the play link
		// and the other an orphan nobody drives, standing in the world for ever
		// and one more of them per teleport.
		//
		// A second registration is now a wasted walk over an empty list rather
		// than a second person, which is the difference between a mistake that
		// costs nothing and one somebody has to photograph to find.
		//
		// **Only teleports are taken.** A subscriber's message has to still be
		// there when a runtime pumps — the delivery pump reads the same list —
		// so everything else is left exactly where the driver put it.
		// **The inbox is re-read every time round and nothing is held across an
		// admission.** Admitting creates instances, and a store that moves its
		// resource storage under a reference taken before the call is a
		// use-after-free waiting for a scene large enough to trigger it.
		for (;;) {
			auto *inbox = store.ResourceMutable<world::Inbox>();
			if (inbox == nullptr) {
				break;
			}

			// **A reply is not an arrival, and the kind alone cannot tell them
			// apart.** `Postbox::Teleport` asks for one, so the *sender's* inbox
			// holds a `BusKind::Teleport` delivery carrying a ticket and no
			// payload. Taken as an arrival it decodes to nothing and builds
			// nobody — but it was still erased from the inbox, so a ticket
			// nothing could then await, and it was counted as an admission in any
			// world that has a `Players` service. `Ticket::Expected` is the field
			// that distinguishes them, exactly as it does in both delivery pumps.
			const auto next =
				std::find_if(inbox->Arrived.begin(), inbox->Arrived.end(), [](const Delivery &delivery) {
					return delivery.Bus == BusKind::Teleport && !delivery.Reply.Expected();
				});
			if (next == inbox->Arrived.end()) {
				break;
			}

			const Delivery taken = *next;
			inbox->Arrived.erase(next);

			AdmitArrival(store, taken);

			// `AdmitArrival` is quiet about a world with no `Players` service —
			// that is the placeholder scene and it takes nobody — so what is
			// counted is what it built rather than what arrived.
			admitted += scene::PlayersOf(store) == ecs::NULL_ENTITY ? 0u : 1u;
		}

		return admitted;
	}

	void RegisterTeleportAdmission(ecs::Scheduler &scheduler) {
		scheduler.Add("teleport.admit", ecs::Phase::PreSimulation, [](ecs::Store &store) {
			(void)AdmitTeleports(store);
		});
	}
}
