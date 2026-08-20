// The connection: handshake, throttle, retry, and the join path that ships
// switched off.
//
// **A fake Discord rather than a real one.** Everything here runs on a machine
// with no Discord installed and no account, which is the only way this is
// checked on CI at all - and it is also the only way the failure paths get
// exercised, because a real client will not close a socket, refuse a handshake
// or stop draining a pipe to order.
//
// Assertions are on the payload **text** rather than on parsed JSON. That is
// not a shortcut: `Vendor::json` is private to the module, and for a protocol
// the bytes are the contract. A test that parsed first would pass on a frame
// whose key was `Cmd`.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <discord/Frame.hpp>
#include <discord/Link.hpp>
#include <memory>
#include <string>
#include <vector>

TEST_SUITE_ID("discord.link")
TEST_DEPENDS("discord.frame")

using discord::Activity;
using discord::DecodedFrame;
using discord::DecodeFrame;
using discord::DecodeResult;
using discord::EncodeFrame;
using discord::Link;
using discord::LinkState;
using discord::MemoryChannel;
using discord::Opcode;
using discord::Settings;

namespace {
	// The other end of the socket, and the only Discord these tests have.
	struct Fake {
		// The channel the link is holding, or null when it has none. Raw
		// because the link owns it; valid only while it is connected.
		MemoryChannel *Live = nullptr;

		// How many times the link has asked for a channel.
		int Attempts = 0;

		// Whether the next attempt finds nothing, which is Discord not running.
		bool Absent = false;

		discord::ChannelSource Source() {
			return [this]() -> std::unique_ptr<discord::Channel> {
				Attempts++;
				if (Absent) {
					Live = nullptr;
					return nullptr;
				}
				auto wire = std::make_unique<MemoryChannel>();
				Live = wire.get();
				return wire;
			};
		}

		// Puts a frame where the link will read it.
		void Say(Opcode op, const std::string &payload) {
			REQUIRE(Live != nullptr);
			const std::vector<std::byte> frame = EncodeFrame(op, payload);
			Live->Readable.insert(Live->Readable.end(), frame.begin(), frame.end());
		}

		void Ready() {
			Say(Opcode::Frame, R"({"cmd":"DISPATCH","evt":"READY","data":{}})");
		}

		// Everything the link has written since this was last called.
		std::vector<DecodedFrame> Heard() {
			std::vector<DecodedFrame> frames;
			if (Live == nullptr) {
				return frames;
			}

			std::vector<std::byte> buffer = std::move(Live->Written);
			Live->Written.clear();

			DecodedFrame frame;
			while (DecodeFrame(buffer, frame) == DecodeResult::Ok) {
				frames.push_back(frame);
			}
			return frames;
		}
	};

	Settings Configured() {
		Settings settings;
		settings.Enabled = true;
		settings.ApplicationId = "123456789012345678";
		return settings;
	}

	Activity Editing(std::string details) {
		Activity activity;
		activity.Details = std::move(details);
		return activity;
	}

	bool Mentions(const std::string &text, std::string_view wanted) {
		return text.find(wanted) != std::string::npos;
	}
}

TEST_CASE("an unconfigured link opens nothing", "[discord][link]") {
	Fake discord;

	SECTION("off") {
		Settings settings = Configured();
		settings.Enabled = false;
		Link link(settings, discord.Source());
		link.Pump(0.0);
		CHECK(link.State() == LinkState::Off);
	}

	SECTION("no application id") {
		Settings settings = Configured();
		settings.ApplicationId.clear();
		Link link(settings, discord.Source());
		link.Pump(0.0);
		CHECK(link.State() == LinkState::Off);
	}

	CHECK(discord.Attempts == 0);
}

TEST_CASE("the first frame is the handshake", "[discord][link]") {
	Fake discord;
	Link link(Configured(), discord.Source());

	link.Pump(0.0);
	REQUIRE(discord.Attempts == 1);
	CHECK(link.State() == LinkState::Handshaking);

	const std::vector<DecodedFrame> heard = discord.Heard();
	REQUIRE(heard.size() == 1);
	CHECK(heard[0].Op == Opcode::Handshake);
	CHECK(Mentions(heard[0].Payload, R"("v":1)"));
	CHECK(Mentions(heard[0].Payload, R"("client_id":"123456789012345678")"));
}

TEST_CASE("READY publishes at once rather than after the interval", "[discord][link]") {
	Fake discord;
	Link link(Configured(), discord.Source());

	link.SetActivity(Editing("Editing Portals"));
	link.Pump(0.0);
	discord.Heard();

	discord.Ready();
	link.Pump(0.1);

	CHECK(link.State() == LinkState::Ready);
	CHECK(link.Sent() == 1);

	const std::vector<DecodedFrame> heard = discord.Heard();
	REQUIRE(heard.size() == 1);
	CHECK(heard[0].Op == Opcode::Frame);
	CHECK(Mentions(heard[0].Payload, R"("cmd":"SET_ACTIVITY")"));
	CHECK(Mentions(heard[0].Payload, R"("details":"Editing Portals")"));
	CHECK(Mentions(heard[0].Payload, R"("pid":)"));
}

TEST_CASE("an unchanged activity is not sent again", "[discord][link]") {
	Fake discord;
	Link link(Configured(), discord.Source());

	link.SetActivity(Editing("Editing Portals"));
	link.Pump(0.0);
	discord.Ready();
	link.Pump(0.1);
	discord.Heard();

	// Every frame for a minute, which is what a caller in a draw loop does.
	for (int frame = 1; frame <= 3600; frame++) {
		link.SetActivity(Editing("Editing Portals"));
		link.Pump(0.1 + static_cast<double>(frame) / 60.0);
	}

	CHECK(link.Sent() == 1);
	CHECK(discord.Heard().empty());
}

TEST_CASE("a changed activity waits for the throttle", "[discord][link]") {
	Fake discord;
	Link link(Configured(), discord.Source());

	link.SetActivity(Editing("one"));
	link.Pump(0.0);
	discord.Ready();
	link.Pump(0.0);
	REQUIRE(link.Sent() == 1);
	discord.Heard();

	link.SetActivity(Editing("two"));

	// Just short of the interval, which is where a diff-only implementation
	// would have sent it.
	link.Pump(discord::MINIMUM_UPDATE_SECONDS - 0.01);
	CHECK(link.Sent() == 1);
	CHECK(discord.Heard().empty());

	link.Pump(discord::MINIMUM_UPDATE_SECONDS);
	CHECK(link.Sent() == 2);
	const std::vector<DecodedFrame> heard = discord.Heard();
	REQUIRE(heard.size() == 1);
	CHECK(Mentions(heard[0].Payload, R"("details":"two")"));
}

TEST_CASE("the last state before the throttle expires is the one sent", "[discord][link]") {
	Fake discord;
	Link link(Configured(), discord.Source());

	link.Pump(0.0);
	discord.Ready();
	link.Pump(0.0);
	discord.Heard();

	// Nine intermediate values inside one interval. A queue would send all of
	// them late; state sends the last one on time.
	for (int step = 1; step <= 9; step++) {
		link.SetActivity(Editing("step " + std::to_string(step)));
		link.Pump(static_cast<double>(step) * 0.1);
	}
	CHECK(discord.Heard().empty());

	link.Pump(discord::MINIMUM_UPDATE_SECONDS);
	const std::vector<DecodedFrame> heard = discord.Heard();
	REQUIRE(heard.size() == 1);
	CHECK(Mentions(heard[0].Payload, R"("details":"step 9")"));
}

TEST_CASE("an empty activity clears the card with null", "[discord][link]") {
	Fake discord;
	Link link(Configured(), discord.Source());

	link.Pump(0.0);
	discord.Heard();
	discord.Ready();
	link.Pump(0.0);

	const std::vector<DecodedFrame> heard = discord.Heard();
	REQUIRE(heard.size() == 1);

	// An empty object leaves the card up with every line blank, which is not
	// the same thing and is what this asserts against.
	CHECK(Mentions(heard[0].Payload, R"("activity":null)"));
}

TEST_CASE("a close returns to backoff and retries on a ladder", "[discord][link]") {
	Fake discord;
	Link link(Configured(), discord.Source());

	link.Pump(0.0);
	discord.Ready();
	link.Pump(0.0);
	REQUIRE(link.State() == LinkState::Ready);

	discord.Say(Opcode::Close, "{}");
	link.Pump(1.0);
	CHECK(link.State() == LinkState::Waiting);
	CHECK(!link.Trouble().empty());

	// The first retry is a second out, so nothing happens before then.
	link.Pump(1.5);
	CHECK(discord.Attempts == 1);

	link.Pump(2.0);
	CHECK(discord.Attempts == 2);
	CHECK(link.State() == LinkState::Handshaking);
}

TEST_CASE("backoff doubles to a ceiling and resets on READY", "[discord][link]") {
	Fake discord;
	discord.Absent = true;
	Link link(Configured(), discord.Source());

	link.Pump(0.0);
	REQUIRE(discord.Attempts == 1);
	CHECK(link.State() == LinkState::Waiting);

	// 1, 2, 4, 8 ... doubling each time nothing answered.
	link.Pump(0.9);
	CHECK(discord.Attempts == 1);
	link.Pump(1.0);
	CHECK(discord.Attempts == 2);
	link.Pump(2.9);
	CHECK(discord.Attempts == 2);
	link.Pump(3.0);
	CHECK(discord.Attempts == 3);

	// Far enough out that the ladder has hit its ceiling.
	double now = 3.0;
	int attempts = discord.Attempts;
	while (discord.Attempts < 12) {
		now += discord::MAXIMUM_RETRY_SECONDS;
		link.Pump(now);
	}
	CHECK(discord.Attempts > attempts);

	// One more, to prove the ceiling holds rather than growing past it.
	attempts = discord.Attempts;
	link.Pump(now + discord::MAXIMUM_RETRY_SECONDS);
	CHECK(discord.Attempts == attempts + 1);

	// Discord starts. The next attempt succeeds and the ladder goes back to
	// the bottom, so a drop after a long outage is not another minute.
	discord.Absent = false;
	now += discord::MAXIMUM_RETRY_SECONDS * 2.0;
	link.Pump(now);
	REQUIRE(link.State() == LinkState::Handshaking);
	discord.Ready();
	link.Pump(now);
	REQUIRE(link.State() == LinkState::Ready);

	discord.Say(Opcode::Close, "{}");
	link.Pump(now);
	attempts = discord.Attempts;
	link.Pump(now + discord::FIRST_RETRY_SECONDS);
	CHECK(discord.Attempts == attempts + 1);
}

TEST_CASE("a handshake nobody answers times out", "[discord][link]") {
	Fake discord;
	Link link(Configured(), discord.Source());

	link.Pump(0.0);
	REQUIRE(link.State() == LinkState::Handshaking);

	link.Pump(discord::HANDSHAKE_TIMEOUT_SECONDS - 0.01);
	CHECK(link.State() == LinkState::Handshaking);

	link.Pump(discord::HANDSHAKE_TIMEOUT_SECONDS);
	CHECK(link.State() == LinkState::Waiting);
	CHECK(!link.Trouble().empty());
}

TEST_CASE("an error before READY is reported and drops the connection", "[discord][link]") {
	Fake discord;
	Link link(Configured(), discord.Source());

	link.Pump(0.0);
	discord.Say(Opcode::Frame, R"({"evt":"ERROR","data":{"code":4000,"message":"Invalid Client ID"}})");
	link.Pump(0.0);

	// The message is Discord's own, because "presence is not working" with a
	// typo'd application id is the case this text exists for.
	CHECK(link.State() == LinkState::Waiting);
	CHECK(link.Trouble() == "Invalid Client ID");
}

TEST_CASE("an error after READY keeps the connection", "[discord][link]") {
	Fake discord;
	Link link(Configured(), discord.Source());

	link.Pump(0.0);
	discord.Ready();
	link.Pump(0.0);
	REQUIRE(link.State() == LinkState::Ready);

	discord.Say(Opcode::Frame, R"({"evt":"ERROR","data":{"message":"Invalid activity"}})");
	link.Pump(0.0);

	CHECK(link.State() == LinkState::Ready);
	CHECK(link.Trouble() == "Invalid activity");
}

TEST_CASE("a ping is echoed as a pong", "[discord][link]") {
	Fake discord;
	Link link(Configured(), discord.Source());

	link.Pump(0.0);
	discord.Ready();
	link.Pump(0.0);
	discord.Heard();

	discord.Say(Opcode::Ping, R"({"nonce":"7"})");
	link.Pump(0.0);

	const std::vector<DecodedFrame> heard = discord.Heard();
	REQUIRE(heard.size() == 1);
	CHECK(heard[0].Op == Opcode::Pong);
	CHECK(heard[0].Payload == R"({"nonce":"7"})");
}

TEST_CASE("a frame that is not JSON is ignored rather than fatal", "[discord][link]") {
	Fake discord;
	Link link(Configured(), discord.Source());

	link.Pump(0.0);
	discord.Ready();
	link.Pump(0.0);
	REQUIRE(link.State() == LinkState::Ready);

	discord.Say(Opcode::Frame, "not json at all");
	link.Pump(0.0);
	CHECK(link.State() == LinkState::Ready);
}

TEST_CASE("a corrupt frame drops the connection", "[discord][link]") {
	Fake discord;
	Link link(Configured(), discord.Source());

	link.Pump(0.0);
	REQUIRE(discord.Live != nullptr);

	// An opcode outside the five. Nothing recovers a framed stream from a
	// header it cannot trust.
	discord.Live->Readable.assign(discord::FRAME_HEADER_BYTES, std::byte{0});
	discord.Live->Readable[0] = std::byte{9};
	link.Pump(0.0);

	CHECK(link.State() == LinkState::Waiting);
}

TEST_CASE("join secrets are absent unless asked for", "[discord][link]") {
	Fake discord;
	Link link(Configured(), discord.Source());

	Activity activity = Editing("Hosting");
	activity.JoinSecret = "a-secret";
	activity.PartySize = 2;
	activity.PartyCapacity = 8;
	link.SetActivity(activity);

	link.Pump(0.0);
	discord.Heard();
	discord.Ready();
	link.Pump(0.0);

	const std::vector<DecodedFrame> heard = discord.Heard();
	REQUIRE(heard.size() == 1);
	CHECK(!Mentions(heard[0].Payload, "SUBSCRIBE"));

	// The party still goes, because "2 of 8" is worth showing on its own. The
	// secret is the half that needs a handler nothing registers yet, and a
	// caller that filled one anyway does not get to advertise an Ask-to-Join
	// nothing can service.
	CHECK(Mentions(heard[0].Payload, R"("party")"));
	CHECK(!Mentions(heard[0].Payload, R"("secrets")"));
}

TEST_CASE("join secrets subscribe, advertise and deliver", "[discord][link]") {
	Fake discord;
	Settings settings = Configured();
	settings.JoinSecrets = true;
	Link link(settings, discord.Source());

	std::string arrived;
	link.OnJoin([&arrived](std::string secret) { arrived = std::move(secret); });

	Activity activity = Editing("Hosting");
	activity.JoinSecret = "a-secret";
	activity.PartyId = "party-1";
	activity.PartySize = 2;
	activity.PartyCapacity = 8;
	link.SetActivity(activity);

	link.Pump(0.0);
	discord.Heard();
	discord.Ready();
	link.Pump(0.0);

	const std::vector<DecodedFrame> heard = discord.Heard();
	REQUIRE(heard.size() == 2);
	CHECK(Mentions(heard[0].Payload, R"("cmd":"SUBSCRIBE")"));
	CHECK(Mentions(heard[0].Payload, R"("evt":"ACTIVITY_JOIN")"));
	CHECK(Mentions(heard[1].Payload, R"("secrets":{"join":"a-secret"})"));

	discord.Say(Opcode::Frame, R"({"evt":"ACTIVITY_JOIN","data":{"secret":"a-secret"}})");
	link.Pump(0.0);
	CHECK(arrived == "a-secret");
}

TEST_CASE("a button is dropped when a join secret is present", "[discord][link]") {
	Fake discord;
	Settings settings = Configured();
	settings.JoinSecrets = true;
	Link link(settings, discord.Source());

	Activity activity = Editing("Hosting");
	activity.Buttons.push_back({"Join this server", "https://example.invalid"});
	link.SetActivity(activity);

	link.Pump(0.0);
	discord.Heard();
	discord.Ready();
	link.Pump(0.0);

	// The SUBSCRIBE, then the activity. This case is about the second.
	std::vector<DecodedFrame> heard = discord.Heard();
	REQUIRE(heard.size() == 2);
	CHECK(Mentions(heard.back().Payload, R"("buttons")"));
	CHECK(!Mentions(heard.back().Payload, R"("secrets")"));

	// Discord draws one row on a card, so the two are exclusive and the join
	// control wins.
	activity.JoinSecret = "a-secret";
	link.SetActivity(activity);
	link.Pump(discord::MINIMUM_UPDATE_SECONDS);

	heard = discord.Heard();
	REQUIRE(heard.size() == 1);
	CHECK(Mentions(heard[0].Payload, R"("secrets")"));
	CHECK(!Mentions(heard[0].Payload, R"("buttons")"));
}

TEST_CASE("changing the application id reconnects and changing wording does not", "[discord][link]") {
	Fake discord;
	Link link(Configured(), discord.Source());

	link.Pump(0.0);
	discord.Ready();
	link.Pump(0.0);
	REQUIRE(link.State() == LinkState::Ready);
	REQUIRE(discord.Attempts == 1);

	// Typing in a settings panel must not drop a connection per keystroke.
	Settings reworded = Configured();
	reworded.Details = "Editing {place}";
	link.Configure(reworded);
	link.Pump(0.0);
	CHECK(link.State() == LinkState::Ready);
	CHECK(discord.Attempts == 1);

	// A socket handshaken as one application cannot start reporting as another.
	Settings renamed = reworded;
	renamed.ApplicationId = "876543210987654321";
	link.Configure(renamed);
	link.Pump(0.0);
	CHECK(discord.Attempts == 2);
	CHECK(link.State() == LinkState::Handshaking);
}

TEST_CASE("turning it off closes the socket", "[discord][link]") {
	Fake discord;
	Link link(Configured(), discord.Source());

	link.Pump(0.0);
	discord.Ready();
	link.Pump(0.0);
	REQUIRE(link.State() == LinkState::Ready);

	Settings off = Configured();
	off.Enabled = false;
	link.Configure(off);
	CHECK(link.State() == LinkState::Off);

	link.Pump(100.0);
	CHECK(discord.Attempts == 1);
	CHECK(link.State() == LinkState::Off);
}

TEST_CASE("a refused write is dropped and re-stated on the next pump", "[discord][link]") {
	Fake discord;
	Link link(Configured(), discord.Source());

	link.SetActivity(Editing("one"));
	link.Pump(0.0);
	discord.Ready();

	// The pipe is not draining. An activity is state, so the update is dropped
	// rather than queued.
	discord.Live->RefuseWrites = true;
	link.Pump(0.0);
	CHECK(link.Sent() == 0);
	CHECK(link.State() == LinkState::Ready);

	discord.Live->RefuseWrites = false;
	link.Pump(discord::MINIMUM_UPDATE_SECONDS);
	CHECK(link.Sent() == 1);

	const std::vector<DecodedFrame> heard = discord.Heard();
	REQUIRE(!heard.empty());
	CHECK(Mentions(heard.back().Payload, R"("details":"one")"));
}

TEST_CASE("a reconnect re-states what a fresh Discord does not know", "[discord][link]") {
	Fake discord;
	Link link(Configured(), discord.Source());

	link.SetActivity(Editing("Editing Portals"));
	link.Pump(0.0);
	discord.Ready();
	link.Pump(0.0);
	REQUIRE(link.Sent() == 1);
	discord.Heard();

	discord.Say(Opcode::Close, "{}");
	link.Pump(0.0);
	REQUIRE(link.State() == LinkState::Waiting);

	// The same activity as before. A link that compared against what it sent
	// to the *previous* socket would decide there was nothing to say, and the
	// card would stay blank until something else changed.
	link.Pump(discord::FIRST_RETRY_SECONDS);
	REQUIRE(link.State() == LinkState::Handshaking);
	discord.Ready();
	link.Pump(discord::FIRST_RETRY_SECONDS);

	CHECK(link.Sent() == 2);
	const std::vector<DecodedFrame> heard = discord.Heard();
	REQUIRE(!heard.empty());
	CHECK(Mentions(heard.back().Payload, R"("details":"Editing Portals")"));
}
