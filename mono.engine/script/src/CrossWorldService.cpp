// The addressed route out of a world, and the channels it is addressed on.
//
// **`MessagingService` is a fan-out and this is a channel**, which is the whole
// distinction: a topic has no destination - right for "the boss died", wrong for
// "world B, here is the score you asked me for" - and the only other operation
// that names a world moves a *person*. So a game wanting to say one thing to one
// world had to broadcast it to everybody or send a player carrying it.
// `world::BusKind::Channel` is the kind, appended beside `Teleport` because a
// channel is a teleport with nobody attached.
//
// **A channel is named, and v0.16's cut was one unnamed pipe per world pair.**
// `Send(world, message)` delivered to a single `MessageReceived` on the far side,
// so every listener in the destination heard everything the pair exchanged and
// had to work out from the payload whether the message was for it. Two
// subsystems talking between one pair of worlds is the ordinary case - a match
// controller and a chat relay, say - and the version where they read each
// other's traffic is one where each has to be written knowing about the other.
//
// So the address is `(world, channel)` on both halves:
//
// - **`OpenChannel(name)` is what a receiver does**, and it hands back the signal
//   for *that channel*. The bus knows what a world is listening for, which is
//   what makes `NoSuchChannel` answerable rather than a delivery that lands
//   somewhere and is discarded.
// - **`SendAsync(world, channel, message)` is what a sender does**, and it
//   suspends on the reply. Every refusal is a `BusStatus` a script reads - see
//   `world::BusStatus` for the table.
//
// **The per-channel signal costs nothing new, which is why it is the shape.** A
// `Connection` has carried a `core::Name` filter since v0.6 for
// `GetPropertyChangedSignal`, and `GetAttributeChangedSignal` already reuses it
// for a name the engine never declared. A channel is the same trick one service
// along: one `SignalKind`, one connection list, and the pump fires the rows whose
// name matches the arrival's. No entity is minted to be a subject and no signal
// kind is added.
//
// **There is no catch-all `MessageReceived` any more, and it could not be kept
// honestly.** A message on a channel this world never opened is refused at the
// bus, so a signal promising "anything addressed to this world" would fire for
// exactly the channels a script had already named - the same set, reached by a
// route where nothing tells it which one arrived. Two ways to do one job, and the
// second one worse.
//
// @tier L9 · shared

#include <engine/script/Codec.hpp>
#include <engine/script/ScriptCall.hpp>
#include <engine/script/ServiceSurface.hpp>
#include <engine/world/Postbox.hpp>

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace engine::script {
	namespace {
		// `CrossWorldService:OpenChannel(name)`
		//
		// **The bus is told before the signal is handed back**, so a script that
		// connects on the returned handle in the same statement is listening on a
		// channel the barrier will actually deliver to. The open takes effect at
		// the next barrier, which is `Postbox::OpenChannel`'s rule and the same
		// one `SubscribeAsync` has: a message sent in the tick the channel opened
		// is refused, because the channel did not exist when it was addressed.
		//
		// **The one refusal this cannot hand back is `TooManyChannels`, and that
		// is the shape of the member rather than an oversight.** The cap is a
		// total the barrier holds - `UniverseSettings::ChannelsPerWorld` - so the
		// verdict arrives a tick later, on the ticket, where every other barrier
		// verdict arrives. This member returns the signal *now* because a script
		// writes `OpenChannel('c'):Connect(...)` as one expression, and making it
		// suspend to collect the verdict would turn it into a promise in
		// JavaScript and a yield in Luau - a different member, not a stricter one.
		// A world that hits the cap is named in the log at the barrier and its
		// senders are told `NoSuchChannel`, which is what a channel that never
		// opened looks like from outside either way.
		void OpenChannel(ScriptCall &call) {
			const std::string channel = call.AsString(0);

			if (!world::Postbox(call.World()).OpenChannel(channel.c_str()).Expected()) {
				// Over budget, named rather than silent - `PublishAsync`'s rule.
				// A channel that quietly failed to open is a receiver that never
				// hears anything and a sender told `NoSuchChannel` for ever.
				call.Raise(("OpenChannel: over this world's budget for '" + channel + "'").c_str());
			}

			call.ReturnSignal(SignalKind::CrossWorldMessage, core::Name(channel));
		}

		// `CrossWorldService:CloseChannel(name)`
		//
		// **Connections on the channel's signal are left alone**, because the
		// script that made them is the only thing that knows whether it is done
		// with them - reopening the channel later must find its handlers still
		// there, which is what a `:Connect` outside any tick means. What stops is
		// the delivery: the bus answers a sender `NoSuchChannel` from the next
		// barrier on.
		void CloseChannel(ScriptCall &call) {
			const std::string channel = call.AsString(0);

			if (!world::Postbox(call.World()).CloseChannel(channel.c_str())) {
				call.Raise(("CloseChannel: over this world's budget for '" + channel + "'").c_str());
			}
		}

		// `CrossWorldService:SendAsync(world, channel, message)`
		//
		// **It suspends, and the previous version's answer was the reason it had
		// to.** `Send` returned a boolean that meant "the budget took it" and
		// threw the bus's own reply away - so `NoSuchWorld` was decided at the
		// barrier, delivered to the sender, matched against nothing, and dropped.
		// A script could not tell a message that arrived from one addressed to a
		// world that had closed, which is precisely the distinction this service
		// exists for beside `MessagingService`.
		//
		// The three values are the store methods': `(value, status, version)` in
		// Luau and `{ Value, Status, Version }` in JavaScript. A channel send
		// carries nothing back, so the value is nil and the status is the answer -
		// `Ok`, `NoSuchWorld`, `NoSuchChannel`, `WorldNotReady` or `Overflow`.
		void SendAsync(ScriptCall &call) {
			const std::string world = call.AsString(0);
			const std::string channel = call.AsString(1);

			ScriptValue value;
			CodecStatus why = CodecStatus::Ok;
			if (!call.ReadValue(2, value, why)) {
				call.Raise((std::string("SendAsync needs a value it can encode: ") + Describe(why)).c_str());
			}

			std::vector<std::byte> bytes;
			if (const CodecStatus encoded = Encode(value, bytes); encoded != CodecStatus::Ok) {
				call.Raise(
					(std::string("SendAsync could not encode the message: ") + Describe(encoded)).c_str()
				);
			}

			const world::Ticket ticket = world::Postbox(call.World()).SendTo(world, channel, bytes);
			if (!ticket.Expected()) {
				// **Over budget is a refusal a script can see**, exactly as it is
				// for a publish: the bus budget is per world per tick and a loop
				// that blew it should be told rather than have its messages
				// disappear. It is raised rather than returned as a status because
				// nothing was sent, so there is no reply to suspend on.
				call.Raise(("SendAsync: over this world's budget for '" + world + "'").c_str());
			}

			call.Await(ticket.Value);
		}

		constexpr std::array<ServiceMethod, 3> CROSS_WORLD_METHODS{{
			{"OpenChannel", OpenChannel},
			{"CloseChannel", CloseChannel},
			{"SendAsync", SendAsync},
		}};
	}

	const ServiceSurface &CrossWorldServiceSurface() {
		static const ServiceSurface SURFACE = [] {
			ServiceSurface surface;
			surface.Name = "CrossWorldService";
			surface.Methods = CROSS_WORLD_METHODS;
			return surface;
		}();
		return SURFACE;
	}
}
