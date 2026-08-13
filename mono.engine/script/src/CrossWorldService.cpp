// The addressed route out of a world.
//
// **`MessagingService` is a fan-out and this is a channel**, which is the whole
// distinction: a topic has no destination — right for "the boss died", wrong for
// "world B, here is the score you asked me for" — and the only other operation
// that names a world moves a *person*. So a game wanting to say one thing to one
// world had to broadcast it to everybody or send a player carrying it.
// `world::BusKind::Channel` is the kind, appended beside `Teleport` because a
// channel is a teleport with nobody attached.
//
// **Its own file since v0.16, out of `Services.cpp`.** It was one function among
// the bus services because it is one, and it moved because it is the first of
// them written once for both languages — leaving it beside eight
// `lua_CFunction`s would have made "which of these has JavaScript" a thing to
// work out by reading rather than a thing the file names.
//
// **`MessageReceived` needed nothing from the neutral layer.** A signal has
// crossed languages since v0.6: `ServiceSignal` names a `SignalKind` and each VM
// already had one way to build a handle onto the shared `SignalTable`. What was
// missing on the JavaScript side was the *delivery* — `PumpJsDeliveries` ignored
// `BusKind::Channel` — which is a pump and not a binding.
//
// @tier L9 · shared

#include "Codec.hpp"
#include "ScriptCall.hpp"
#include "ServiceSurface.hpp"

#include <engine/world/Postbox.hpp>

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace engine::script {
	namespace {
		// `CrossWorldService:Send(worldName, message)`
		//
		// **Answers the status rather than raising**, matching `PublishAsync`
		// beside it: a world that is not running is `NoSuchWorld`, which a caller
		// can act on. That is also the closest thing to `GetWorlds` this service
		// has — see its declaration for why the real one needs machinery that
		// does not exist yet.
		void CrossWorldSend(ScriptCall &call) {
			const std::string world = call.AsString(0);

			ScriptValue value;
			CodecStatus why = CodecStatus::Ok;
			if (!call.ReadValue(1, value, why)) {
				call.Raise((std::string("Send needs a value it can encode: ") + Describe(why)).c_str());
			}

			std::vector<std::byte> bytes;
			if (const CodecStatus encoded = Encode(value, bytes); encoded != CodecStatus::Ok) {
				call.Raise((std::string("Send could not encode the message: ") + Describe(encoded)).c_str());
			}

			world::Postbox box(call.World());
			const world::Ticket ticket = box.SendTo(world, bytes);

			// **Over budget is a refusal a script can see**, exactly as it is for
			// a publish: the bus budget is per world per tick and a loop that
			// blew it should be told rather than have its messages disappear.
			call.ReturnBoolean(ticket.Expected());
		}

		constexpr std::array<ServiceMethod, 1> METHODS{{
			{"Send", CrossWorldSend},
		}};

		constexpr std::array<ServiceSignal, 1> SIGNALS{{
			{"MessageReceived", SignalKind::CrossWorldMessage},
		}};
	}

	const ServiceSurface &CrossWorldServiceSurface() {
		static const ServiceSurface SURFACE = [] {
			ServiceSurface surface;
			surface.Name = "CrossWorldService";
			surface.Methods = METHODS;
			surface.Signals = SIGNALS;
			return surface;
		}();
		return SURFACE;
	}
}
