#pragma once

// One virtual client, and the rule that says what it is doing.
//
// **A genuine session and not a traffic generator.** Everything below the
// `Connector` is the real thing - a real UDP socket, `net::Handshake`'s X25519
// agreement, `net::Cookie`'s challenge, `replication::Admission`'s four
// messages, ChaCha20-Poly1305 on every payload after them, and a real
// `ecs::Store` that the snapshot and every delta are applied into. A harness
// that stubbed any of those would measure a server nobody runs: the admission
// arithmetic is the expensive half of a join, and the apply is what decides
// whether a client can keep up.
//
// **The phase rule is a free function**, so the state machine is testable
// without a socket, a server or a clock - which is the half of this tool that
// can regress silently. What is left in `Session` is the plumbing.
//
// @tier shared

#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/net/Endpoint.hpp>
#include <engine/net/Transport.hpp>
#include <engine/replication/Connector.hpp>

#include <cstdint>
#include <memory>

namespace loadtest {

	// How far a virtual client has got.
	//
	// One way, and `Refused` and `TimedOut` are terminal - the same shape
	// `net::Link`'s lifecycle has, and for the same reason: a session that could
	// come back to life is one every counter has to be re-checked against.
	enum class Stage : uint8_t {
		Dialling,  ///< The handshake is in flight. Nothing has been admitted.
		Streaming, ///< Admitted. The join snapshot is arriving in chunks.
		Playing,   ///< Joined. The whole world arrived and inputs are going up.
		Refused,   ///< The server turned this client away. Terminal.
		TimedOut,  ///< Nothing moved inside the deadline. Terminal.
	};

	// A short name for a stage, for a report line.
	//
	// @param stage The stage to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(Stage stage);

	// What a connector says about itself, as the phase rule reads it.
	//
	// Three booleans rather than the connector, so the rule can be exercised
	// over combinations a real server would take a minute to produce.
	struct Progress {
		bool Rejected = false; ///< `Connector::Rejected`.
		bool Admitted = false; ///< `Connector::Admitted`.
		bool Joined = false;   ///< `Connector::Joined`.
	};

	// The stage a session moves to.
	//
	// **The deadline is measured from the last stage change and not from the
	// start**, which is what makes it a stall detector rather than a total
	// budget. A join is chunked across ticks and a two-hundred-client server
	// legitimately takes many seconds to finish one; what says something is
	// wrong is a client that stops making progress at all.
	//
	// @param current          Where the session is now.
	// @param progress         What the connector reports.
	// @param waitedSeconds    Seconds since this session last changed stage.
	// @param deadlineSeconds  How long a stage may go nowhere. Zero or less
	//                         disables the timeout, which is what a debugging
	//                         run wants.
	// @return The stage to move to, which is `current` when nothing changed.
	Stage NextStage(Stage current, const Progress &progress, double waitedSeconds, double deadlineSeconds);

	// Whether a stage is one nothing leaves.
	//
	// @param stage The stage.
	// @return `true` for `Refused` and `TimedOut`.
	bool Terminal(Stage stage);

	// What one virtual client did, gathered when the run ends.
	struct SessionReport {
		Stage Final = Stage::Dialling;

		// Seconds from the first datagram to admission and to the join. Zero for
		// a session that never got there, which is why the stage is read first.
		//@{
		double AdmitSeconds = 0.0;
		double JoinSeconds = 0.0;
		//@}

		// The link's own counters, which are measured at the socket.
		//@{
		uint64_t BytesSent = 0;
		uint64_t BytesReceived = 0;
		uint64_t PacketsSent = 0;
		uint64_t PacketsReceived = 0;
		uint64_t PacketsLost = 0;
		//@}

		// Inputs this client submitted, and the ones the link refused.
		//
		// A refusal is the per-tick packet or byte budget saying no, which is
		// ordinary backpressure rather than an error - but a client whose inputs
		// are mostly refused is a client the server is not hearing from.
		//@{
		uint64_t InputsSent = 0;
		uint64_t InputsRefused = 0;
		//@}

		// What the replica underneath saw.
		//@{
		uint64_t Applied = 0;	 ///< Ticks applied in full.
		uint64_t Deltas = 0;	 ///< Deltas applied.
		uint64_t Incomplete = 0; ///< Ticks a part of never arrived.
		uint64_t Refusals = 0;	 ///< Inbound datagrams that would not parse.
		//@}

		// Time inside `Connector::Poll` - decrypting, parsing and applying.
		//
		// **The client-side cost, and the number that says whether the harness
		// itself is the bottleneck.** If this is most of a harness tick then the
		// run is measuring this program rather than the server.
		//@{
		double ApplyMicroseconds = 0.0;
		uint64_t Polls = 0;
		//@}

		// Entities in the replica when the run ended. A world this client can
		// see none of is a run that proved nothing.
		uint64_t Entities = 0;
	};

	// How a virtual client is set up.
	struct SessionSettings {
		// Where the server is.
		engine::net::Endpoint Server;

		// How long a stage may make no progress before the session is written
		// off. See `NextStage`.
		double StallSeconds = 20.0;

		// How often this client submits an input, in harness ticks. One is every
		// tick, which is what a real client does.
		uint32_t InputEveryTicks = 1;

		// Which way this client walks, as an angle in radians. Handed in rather
		// than drawn from a random source, for `net::LossyTransport`'s reason: a
		// run has to be reproducible from its inputs alone.
		float HeadingRadians = 0.0f;
	};

	// One virtual client: a socket, a connector, and a replica of the world.
	//
	// Not copyable, because it owns a socket. Held by pointer in the harness for
	// the same reason.
	class Session {
	  public:
		// Opens a socket and starts dialling.
		//
		// @param settings   Where to dial and how to behave.
		// @param nowSeconds The current time.
		Session(const SessionSettings &settings, double nowSeconds);

		~Session();

		Session(const Session &) = delete;
		Session &operator=(const Session &) = delete;
		Session(Session &&) = delete;
		Session &operator=(Session &&) = delete;

		// Whether the socket was bound. A session that was not never dials.
		bool Open() const {
			return Wire != nullptr;
		}

		// One client tick: take everything that arrived, submit an input, and
		// advance the link.
		//
		// @param nowSeconds The current time.
		// @param tick       The harness tick, for the input cadence.
		void Tick(double nowSeconds, uint64_t tick);

		// Where this session has got to.
		Stage Phase() const {
			return Stage_;
		}

		// What this session did, as of now.
		//
		// Not `const`, because the link's own counters are reached through a
		// mutable accessor - `net::Link` is a state machine rather than a record.
		SessionReport Report();

		// The replica, for a caller that wants to look at the world.
		const engine::ecs::Store &World() const {
			return Store_;
		}

	  private:
		void Submit(double nowSeconds);

		SessionSettings Settings;

		std::unique_ptr<engine::net::Transport> Wire;
		std::unique_ptr<engine::replication::Connector> Link;

		engine::ecs::Store Store_;

		// The `Player` the server said is this client's, or null until it does.
		// The one fact a replica cannot derive - it arrives as a per-client user
		// message rather than as replicated state.
		engine::ecs::Entity Mine;

		Stage Stage_ = Stage::Dialling;

		double StartedAt = 0.0;
		double StageChangedAt = 0.0;
		double AdmittedAt = 0.0;
		double JoinedAt = 0.0;

		uint64_t InputsSent = 0;
		uint64_t InputsRefused = 0;
		double ApplyMicroseconds = 0.0;
		uint64_t Polls = 0;
	};

	// Registers every component and class a replica of this engine's world
	// needs, once per process.
	//
	// **The same set `client::BuildReplicatedWorld` registers, less the parts
	// that only draw.** A snapshot naming a component this build has not
	// registered is refused whole rather than half-merged, so a harness
	// registering less than a client does is a harness measuring a join that
	// never completes.
	void RegisterReplicaTypes();
}
