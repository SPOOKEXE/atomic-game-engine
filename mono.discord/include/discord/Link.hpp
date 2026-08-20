#pragma once

// The connection to Discord: opening it, handshaking, retrying, throttling and
// saying what is true now.
//
// **Deliberately not called `Presence`.** `network::Presence` already exists
// and means announcing on a subnet and registering at a rendezvous point. The
// studio includes both headers, and two `Presence` types in one translation
// unit is a trap for whoever reads it next.
//
// ## Nothing blocks, and nothing threads
//
// The socket is non-blocking and a write that cannot go out is dropped. That is
// safe here and would not be in most protocols, because an activity is a
// statement of what is true rather than an event in a stream - see
// `Activity.hpp`. The next pump re-states it. A studio frame must never stall
// because Discord stopped draining a pipe.
//
// ## Time is passed in
//
// Every deadline is an argument, which is `engine::net`'s rule inherited
// through `mono.network`. A suite states a timeout instead of waiting for one,
// so the whole retry ladder is exercised in microseconds.
//
// @tier shared
// @since v0.17

#include <discord/Activity.hpp>
#include <discord/Channel.hpp>
#include <discord/Settings.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace discord {

	// `Frame.hpp`, declared rather than included.
	//
	// Both are named only by private members below, and four programs include
	// this header. Pulling the wire codec into every one of them to describe
	// two arguments they cannot call would be widening a public header for
	// nothing, which `AGENTS.md` names directly.
	//@{
	enum class Opcode : uint32_t;
	struct DecodedFrame;
	//@}

	// The shortest gap between two updates.
	//
	// Discord accepts about five updates per twenty seconds. Four seconds
	// apart is that budget spent evenly rather than in a burst followed by
	// silence.
	inline constexpr double MINIMUM_UPDATE_SECONDS = 4.0;

	// How long to wait before the first retry, and the ceiling it doubles to.
	//@{
	inline constexpr double FIRST_RETRY_SECONDS = 1.0;
	inline constexpr double MAXIMUM_RETRY_SECONDS = 60.0;
	//@}

	// How long a handshake may go unanswered before the socket is dropped.
	//
	// A socket that accepts and then says nothing is the shape a wrong
	// application id takes, and without this the link would sit in
	// `Handshaking` forever showing nothing and reporting no fault.
	inline constexpr double HANDSHAKE_TIMEOUT_SECONDS = 10.0;

	// Where a link has got to.
	//
	// @since v0.17
	enum class LinkState : uint8_t {
		// Nothing configured, or configured off. No socket is opened.
		Off = 0,

		// Configured, with no connection. Either Discord is not running or a
		// retry is pending.
		Waiting = 1,

		// Connected, handshake sent, no answer yet.
		Handshaking = 2,

		// Discord answered `READY`. Updates are going out.
		Ready = 3,
	};

	// A stable, human-readable name for a state.
	//
	// @param state Which one.
	// @return A view valid for the lifetime of the process.
	// @since v0.17
	const char *Describe(LinkState state);

	// Opens a channel to Discord. Replaced in tests.
	//
	// @since v0.17
	using ChannelSource = std::function<std::unique_ptr<Channel>()>;

	// The connection, and everything that keeps it alive.
	//
	// @since v0.17
	class Link {
	  public:
		// @param settings What to report and whether to report it.
		explicit Link(Settings settings);

		// @param settings What to report.
		// @param source   Where a channel comes from. For tests, and for an
		//        install with a socket in a place `SocketCandidates` misses.
		Link(Settings settings, ChannelSource source);

		~Link();

		Link(const Link &) = delete;
		Link &operator=(const Link &) = delete;

		// Replaces what is configured.
		//
		// **Drops the connection when the identity changed**, because a socket
		// handshaken as one application cannot start reporting as another.
		// Changing a template does not, so editing text in the tab does not
		// flicker somebody's profile.
		//
		// @param settings The new settings.
		void Configure(const Settings &settings);

		// What is configured now.
		//
		// @return The settings.
		const Settings &Configured() const {
			return Wanted;
		}

		// Replaces what is reported.
		//
		// Safe to call every frame: nothing is sent until it differs from what
		// went out last and the throttle allows another.
		//
		// @param activity What is true now.
		void SetActivity(const Activity &activity);

		// Connects, handshakes, retries, drains, and sends what is due.
		//
		// @param nowSeconds A monotonic clock, in seconds. The same one every
		//        call; the origin does not matter.
		void Pump(double nowSeconds);

		// Where the connection has got to.
		//
		// @return The state.
		LinkState State() const {
			return Where;
		}

		// What went wrong last, for a panel to show.
		//
		// **Not cleared by a successful connection**, because the thing worth
		// reading after a reconnect is usually why it dropped. It is cleared
		// when `READY` arrives, which is the point at which the last fault
		// stopped being the current story.
		//
		// @return The message, or empty when nothing has gone wrong.
		const std::string &Trouble() const {
			return Fault;
		}

		// How many updates have reached Discord, for a panel and for a test.
		//
		// @return The count since this link was made.
		uint64_t Sent() const {
			return Updates;
		}

		// Called with a join secret when somebody accepts an invitation.
		//
		// Only ever called with `Settings::JoinSecrets` on.
		//
		// @param handler What to do with the secret.
		void OnJoin(std::function<void(std::string)> handler);

	  private:
		// Drops the socket and schedules the next attempt.
		void Drop(double nowSeconds, std::string why);

		// Opens a socket and sends the handshake.
		void Attempt(double nowSeconds);

		// Reads what has arrived and acts on every whole frame in it.
		void Drain(double nowSeconds);

		// Acts on one frame.
		void Handle(const DecodedFrame &frame, double nowSeconds);

		// Writes the current activity, if the throttle allows it.
		void Publish(double nowSeconds);

		// Writes one frame, answering whether it went.
		bool Write(Opcode op, const std::string &payload);

		Settings Wanted;
		ChannelSource Source;
		std::unique_ptr<Channel> Wire;

		LinkState Where = LinkState::Off;
		std::string Fault;

		Activity Saying;
		Activity LastSent;
		bool Dirty = false;

		// Accumulates between reads, because a frame may arrive in pieces.
		std::vector<std::byte> Incoming;

		double NextAttemptSeconds = 0.0;
		double RetrySeconds = FIRST_RETRY_SECONDS;
		double HandshakeDeadline = 0.0;
		double LastUpdateSeconds = 0.0;
		bool Attempted = false;

		uint64_t Nonce = 0;
		uint64_t Updates = 0;

		std::function<void(std::string)> Joined;
	};
}
