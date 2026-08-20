#include "Platform.hpp"

#include <engine/core/Log.hpp>

#include <algorithm>
#include <discord/Frame.hpp>
#include <discord/Link.hpp>
#include <nlohmann/json.hpp>
#include <utility>

namespace discord {

	using nlohmann::json;

	const char *Describe(LinkState state) {
		switch (state) {
		case LinkState::Off:
			return "off";
		case LinkState::Waiting:
			return "waiting for Discord";
		case LinkState::Handshaking:
			return "connecting";
		case LinkState::Ready:
			return "reporting";
		}
		// No default label, so adding a state is a warning here.
		return "?";
	}

	namespace {
		// The activity, as Discord's RPC wants it.
		//
		// **Absent rather than empty for everything optional.** Discord reads a
		// present-but-empty `details` as a line to draw, so a card configured
		// with one line would get a blank second one and look broken.
		// @param activity What to say.
		// @param mayJoin  Whether `Settings::JoinSecrets` is on. A secret is
		//        dropped when it is not, because advertising an Ask-to-Join
		//        that nothing can service is worse than not offering one.
		json Payload(const Activity &activity, bool mayJoin) {
			json body = json::object();

			if (!activity.Details.empty()) {
				body["details"] = activity.Details;
			}
			if (!activity.State.empty()) {
				body["state"] = activity.State;
			}
			if (activity.StartedUnixSeconds > 0) {
				body["timestamps"] = json{{"start", activity.StartedUnixSeconds}};
			}
			if (!activity.LargeImage.empty()) {
				json assets{{"large_image", activity.LargeImage}};
				if (!activity.LargeText.empty()) {
					assets["large_text"] = activity.LargeText;
				}
				body["assets"] = std::move(assets);
			}

			if (activity.PartySize > 0) {
				// **Capacity is at least the size.** Discord draws "3 of 2" for
				// the other order rather than refusing it, and a host that
				// undercounted its own capacity would rather show a full party.
				const uint32_t capacity = std::max(activity.PartyCapacity, activity.PartySize);
				body["party"] = json{
					{"id", activity.PartyId},
					{"size", json::array({activity.PartySize, capacity})},
				};
			}

			// **Buttons and a join secret are exclusive, and the secret wins.**
			// Discord draws one row on a card: either the buttons an author
			// wrote or the join control it owns. Sending both gets one of them
			// silently dropped, and which one is not ours to decide - so the
			// real join, when it is switched on, is the one that survives.
			const bool joining = mayJoin && !activity.JoinSecret.empty();
			if (joining) {
				body["secrets"] = json{{"join", activity.JoinSecret}};
			} else if (!activity.Buttons.empty()) {
				json buttons = json::array();
				for (const Button &button : activity.Buttons) {
					if (buttons.size() >= MAXIMUM_BUTTONS) {
						break;
					}
					if (button.Label.empty() || button.Url.empty()) {
						continue;
					}
					buttons.push_back(json{{"label", button.Label}, {"url", button.Url}});
				}
				if (!buttons.empty()) {
					body["buttons"] = std::move(buttons);
				}
			}

			return body;
		}
	}

	Link::Link(Settings settings) : Link(std::move(settings), [] { return ConnectLocal(); }) {}

	Link::Link(Settings settings, ChannelSource source)
		: Wanted(std::move(settings)), Source(std::move(source)) {
		Where = IsConfigured(Wanted) ? LinkState::Waiting : LinkState::Off;
	}

	Link::~Link() = default;

	void Link::Configure(const Settings &settings) {
		// **Only the identity forces a reconnect.** A socket handshaken as one
		// application cannot start reporting as another, and subscribing to the
		// join event happens once on `READY`. Everything else is wording, which
		// the caller turns into an `Activity` - so typing in the tab re-renders
		// a card rather than dropping a connection per keystroke.
		const bool identityChanged =
			settings.ApplicationId != Wanted.ApplicationId || settings.JoinSecrets != Wanted.JoinSecrets;
		const bool wasConfigured = IsConfigured(Wanted);

		Wanted = settings;

		if (!IsConfigured(Wanted)) {
			if (Wire) {
				Wire->Close();
				Wire.reset();
			}
			Incoming.clear();
			Where = LinkState::Off;
			return;
		}

		if (identityChanged || !wasConfigured) {
			if (Wire) {
				Wire->Close();
				Wire.reset();
			}
			Incoming.clear();
			Where = LinkState::Waiting;
			Fault.clear();

			// Somebody just corrected the id in a settings panel. Making them
			// wait out a backoff that grew while it was wrong would look like
			// the correction did not take.
			RetrySeconds = FIRST_RETRY_SECONDS;
			Attempted = false;
		}
	}

	void Link::SetActivity(const Activity &activity) {
		Saying = activity;
		Dirty = !(Saying == LastSent);
	}

	void Link::OnJoin(std::function<void(std::string)> handler) {
		Joined = std::move(handler);
	}

	void Link::Pump(double nowSeconds) {
		if (!IsConfigured(Wanted)) {
			if (Wire) {
				Wire->Close();
				Wire.reset();
				Incoming.clear();
			}
			Where = LinkState::Off;
			return;
		}

		if (!Wire) {
			if (!Attempted || nowSeconds >= NextAttemptSeconds) {
				Attempt(nowSeconds);
			}
			if (!Wire) {
				return;
			}
		}

		Drain(nowSeconds);
		if (!Wire) {
			return;
		}

		// A socket that accepts and then says nothing is the shape a wrong
		// application id takes. Without this the link would sit here forever,
		// showing nothing and reporting no fault.
		if (Where == LinkState::Handshaking && nowSeconds >= HandshakeDeadline) {
			Drop(nowSeconds, "Discord did not answer the handshake");
			return;
		}

		Publish(nowSeconds);
	}

	void Link::Attempt(double nowSeconds) {
		Attempted = true;
		Where = LinkState::Waiting;
		Wire = Source ? Source() : nullptr;

		if (!Wire) {
			// **`TRACE`, and this is the line that decides it.** A headless
			// origin runs for months beside no Discord at all, so anything
			// louder is a log file made of one sentence repeated.
			ENGINE_TRACE("discord: nothing answered; retrying in {:.0f}s", RetrySeconds);
			NextAttemptSeconds = nowSeconds + RetrySeconds;
			RetrySeconds = std::min(RetrySeconds * 2.0, MAXIMUM_RETRY_SECONDS);
			return;
		}

		Incoming.clear();

		const json hello{{"v", 1}, {"client_id", Wanted.ApplicationId}};
		if (!Write(Opcode::Handshake, hello.dump())) {
			Drop(nowSeconds, "could not send the handshake");
			return;
		}

		Where = LinkState::Handshaking;
		HandshakeDeadline = nowSeconds + HANDSHAKE_TIMEOUT_SECONDS;
	}

	void Link::Drop(double nowSeconds, std::string why) {
		if (Wire) {
			Wire->Close();
			Wire.reset();
		}
		Incoming.clear();
		Where = LinkState::Waiting;
		Fault = std::move(why);

		// **What was last sent is forgotten with the socket.** A reconnected
		// Discord knows nothing about this process, so the comparison that
		// keeps the link quiet has to start again or the first thing it would
		// decide is that there is nothing to say.
		LastSent = Activity{};
		Dirty = true;

		ENGINE_TRACE("discord: {}; retrying in {:.0f}s", Fault, RetrySeconds);
		NextAttemptSeconds = nowSeconds + RetrySeconds;
		RetrySeconds = std::min(RetrySeconds * 2.0, MAXIMUM_RETRY_SECONDS);
	}

	void Link::Drain(double nowSeconds) {
		const ChannelStatus status = Wire->Receive(Incoming);

		DecodedFrame frame;
		for (;;) {
			const DecodeResult result = DecodeFrame(Incoming, frame);
			if (result == DecodeResult::Incomplete) {
				break;
			}
			if (result == DecodeResult::Corrupt) {
				Drop(nowSeconds, "a frame from Discord did not make sense");
				return;
			}

			Handle(frame, nowSeconds);
			if (!Wire) {
				// Handling it closed the connection. Anything still buffered
				// belongs to a socket that no longer exists.
				return;
			}
		}

		// Checked after draining rather than before, so a `Close` frame that
		// arrived in the same read as the hang-up is still acted on.
		if (status == ChannelStatus::Closed) {
			Drop(nowSeconds, "Discord closed the connection");
		}
	}

	void Link::Handle(const DecodedFrame &frame, double nowSeconds) {
		switch (frame.Op) {
		case Opcode::Ping:
			// Echoed exactly, which is what a pong is.
			Write(Opcode::Pong, frame.Payload);
			return;
		case Opcode::Pong:
			return;
		case Opcode::Close:
			Drop(nowSeconds, "Discord closed the connection");
			return;
		case Opcode::Handshake:
			// Only ever sent by us. A Discord that sent one is not one this
			// understands, so it is ignored rather than guessed at.
			return;
		case Opcode::Frame:
			break;
		}

		const json message = json::parse(frame.Payload, nullptr, false);
		if (message.is_discarded() || !message.is_object()) {
			return;
		}

		const auto event = message.find("evt");
		if (event == message.end() || !event->is_string()) {
			// A command's acknowledgement. Nothing here waits on one: an
			// activity is state, and the next pump re-states it if this one did
			// not land.
			return;
		}
		const std::string what = event->get<std::string>();

		if (what == "READY") {
			Where = LinkState::Ready;
			RetrySeconds = FIRST_RETRY_SECONDS;
			Fault.clear();
			ENGINE_INFO("discord: reporting as application {}", Wanted.ApplicationId);

			if (Wanted.JoinSecrets) {
				const json subscribe{
					{"cmd", "SUBSCRIBE"},
					{"evt", "ACTIVITY_JOIN"},
					{"nonce", std::to_string(++Nonce)},
					{"args", json::object()},
				};
				Write(Opcode::Frame, subscribe.dump());
			}

			// The first update goes out now rather than one interval from now.
			// A card that took four seconds to appear after connecting reads as
			// a feature that half works.
			LastUpdateSeconds = nowSeconds - MINIMUM_UPDATE_SECONDS;
			Dirty = true;
			Publish(nowSeconds);
			return;
		}

		if (what == "ERROR") {
			std::string said = "Discord refused the connection";
			if (const auto data = message.find("data"); data != message.end() && data->is_object()) {
				if (const auto text = data->find("message"); text != data->end() && text->is_string()) {
					said = text->get<std::string>();
				}
			}

			// **An error before `READY` is fatal to the connection and one
			// after it is not.** The first is almost always a wrong application
			// id, which no amount of waiting fixes and which the tab needs to
			// say out loud; the second is one activity Discord did not like,
			// and the socket is still good.
			if (Where != LinkState::Ready) {
				Drop(nowSeconds, said);
			} else {
				Fault = said;
				ENGINE_WARN("discord: {}", Fault);
			}
			return;
		}

		if (what == "ACTIVITY_JOIN" && Wanted.JoinSecrets && Joined) {
			if (const auto data = message.find("data"); data != message.end() && data->is_object()) {
				if (const auto secret = data->find("secret"); secret != data->end() && secret->is_string()) {
					Joined(secret->get<std::string>());
				}
			}
		}
	}

	void Link::Publish(double nowSeconds) {
		if (Where != LinkState::Ready || !Dirty) {
			return;
		}
		if (nowSeconds - LastUpdateSeconds < MINIMUM_UPDATE_SECONDS) {
			return;
		}

		// **`null` rather than an empty object when there is nothing to say.**
		// That is how this protocol clears a presence; an empty object leaves
		// the card up with every line blank.
		const json message{
			{"cmd", "SET_ACTIVITY"},
			{"nonce", std::to_string(++Nonce)},
			{"args",
			 json{
				 {"pid", ProcessId()},
				 {"activity", Saying.IsEmpty() ? json(nullptr) : Payload(Saying, Wanted.JoinSecrets)},
			 }},
		};

		if (!Write(Opcode::Frame, message.dump())) {
			// `Send` closes the channel itself when a frame went out half
			// written, which is the only unrecoverable case. Anything else is a
			// refusal to take it now, and the next pump says the same thing
			// again.
			if (Wire && !Wire->Open()) {
				Drop(nowSeconds, "the connection went while an update was going out");
			}
			return;
		}

		LastSent = Saying;
		Dirty = false;
		LastUpdateSeconds = nowSeconds;
		Updates++;
	}

	bool Link::Write(Opcode op, const std::string &payload) {
		if (!Wire) {
			return false;
		}

		const std::vector<std::byte> frame = EncodeFrame(op, payload);
		if (frame.empty()) {
			ENGINE_WARN("discord: an update was too large to send and was dropped");
			return false;
		}
		return Wire->Send(frame) == ChannelStatus::Ok;
	}
}
