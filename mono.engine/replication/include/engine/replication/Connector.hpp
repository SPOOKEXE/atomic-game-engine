#pragma once

// @tier L12 · shared

#include <engine/ecs/Store.hpp>
#include <engine/net/Cookie.hpp>
#include <engine/net/Endpoint.hpp>
#include <engine/net/Handshake.hpp>
#include <engine/net/Transport.hpp>
#include <engine/replication/Admission.hpp>
#include <engine/replication/Prediction.hpp>
#include <engine/replication/Replica.hpp>
#include <engine/replication/Session.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace engine::replication {

	// How a connector talks to its server.
	//
	// @since v0.3
	struct ConnectorSettings {
		// The server's public key, or nothing to accept any server.
		// @since v0.9
		std::optional<assets::PublicKey> ServerIdentity;

		// This client's own signing key, or null to prove nothing.
		// @since v0.9
		const assets::SigningKey *ClientIdentity = nullptr;

		// The link's own settings — framing, timeouts and per-tick budgets.
		SessionSettings Session;

		// How much unacknowledged input this client keeps for replay.
		PredictionSettings Prediction;

		// How often to say the same thing again while the exchange is unfinished.
		//
		double RepeatEverySeconds = 0.25;
	};

	// One connection to an authoritative server, and the replica it feeds.
	//
	// @since v0.3
	class Connector {
	  public:
		// Opens a connection to a server.
		//
		// @param transport  The wire. Borrowed, not owned.
		// @param server     Where the server is.
		// @param nowSeconds The current time.
		// @param settings   How to frame, resend and predict.
		Connector(
			net::Transport &transport,
			const net::Endpoint &server,
			double nowSeconds,
			const ConnectorSettings &settings = {}
		);

		// Takes everything waiting, applies it, and acknowledges.
		//
		// @param store      The replica world. Written into.
		// @param nowSeconds The current time.
		void Poll(ecs::Store &store, double nowSeconds);

		// Advances the link and resets its per-tick budget.
		//
		// @param nowSeconds The current time.
		void Advance(double nowSeconds);

		// Sends what the player did this tick.
		// @param tick       The tick the input was produced for.
		// @param bytes      The game's encoding.
		// @param nowSeconds The current time.
		// @return `false` when the link refused it.
		bool Submit(uint64_t tick, std::span<const std::byte> bytes, double nowSeconds);

		// Whether the key exchange finished and the server let this client in.
		// @return `true` once admitted.
		bool Admitted() const {
			return Phase == Stage::Admitted;
		}

		// Sends a message this module does not read.
		//
		// `Listener::SendTo`'s twin — see the note there on why widening this
		// pair beats standing up a fourth session type.
		//
		// @param message    The payload.
		// @param nowSeconds The current time.
		// @return `false` when the link refused it, including before admission:
		//         there is no session to carry it on until then, and queueing
		//         one would be an outbox this module deliberately does not keep.
		// @since v0.13
		bool SendUser(std::span<const std::byte> message, double nowSeconds);

		// Hears the messages this module does not read.
		//
		// Called from `Poll`, on the thread that called it.
		//
		// @param handler Called as `handler(payload)`.
		// @since v0.13
		void OnUserMessage(std::function<void(std::span<const std::byte>)> handler);

		// Offers every datagram to somebody else before this connector reads it.
		//
		// `Listener::SetForeign`'s twin, and it exists for exactly the same
		// physical reason: a NAT mapping belongs to a port, so a client that
		// punched a hole on some other socket punched a hole to a socket its
		// server will never send to. Both ends of a peer-to-peer session run
		// their rendezvous traffic over the port the session itself uses.
		//
		// Consulted before the source check, because a rendezvous message
		// arrives from a coordination point rather than from the server.
		//
		// @param handler Called as `handler(datagram, from)`, returning whether
		//        it took the datagram. An empty handler is the default.
		// @since v0.13
		void SetForeign(std::function<bool(std::span<const std::byte>, const net::Endpoint &)> handler);

		// Whether the exchange failed and this connector will not retry.
		// @return `true` when the exchange is over and failed.
		bool Rejected() const {
			return Phase == Stage::Refused;
		}

		// Whether the full snapshot has arrived and been applied.
		// @return `true` once joined.
		bool Joined() const {
			return Replica_.Joined();
		}

		// The last tick applied in full.
		//
		// @return The tick, or zero before joining.
		uint64_t Applied() const {
			return Replica_.Applied();
		}

		// The inputs the server has not yet confirmed consuming.
		//
		// @return The unacknowledged inputs, oldest first.
		std::span<const Input> Unconfirmed() const {
			return Prediction_.Pending();
		}

		// Entities the server said to stop drawing.
		//
		// @return The entities, valid until the next `Poll`.
		std::span<const ecs::Entity> Forgotten() const {
			return Replica_.Forgotten();
		}

		// Drops the forgotten list, once the caller has acted on it.
		void ClearForgotten() {
			Replica_.ClearForgotten();
		}

		// The link's state machine.
		//
		// @return The link.
		net::Link &Link() {
			return Wire.Link();
		}

		// What this connection has done.
		//
		// @since v0.3
		struct Statistics {
			// Inbound datagrams this connection would not parse.
			//
			// **Every field of an inbound message is hostile**, so a refusal here
			// is the ordinary outcome of a hostile or corrupt packet rather than
			// a fault to investigate — it is the count *rising steadily* that
			// says something.
			uint64_t Refused = 0;

			// Ticks applied to the replica in full.
			uint64_t Applied = 0;
		};

		// What this connection has done.
		//
		// @return The statistics.
		const Statistics &Stats() const {
			return Stats_;
		}

		// What the replica underneath has seen.
		//
		// @return The replica's statistics.
		const Replica::Statistics &ReplicaStats() const {
			return Replica_.Stats();
		}

	  private:
		// How far through the admission exchange this end is.
		//
		enum class Stage : uint8_t {
			Greeting,  ///< The hello is going out; no cookie yet.
			Answering, ///< A cookie arrived; the answer is going out.
			Admitted,  ///< The welcome verified. The link is `Connected`.
			Refused,   ///< The exchange failed. Terminal.
		};

		void Repeat(double nowSeconds);
		void Consume(std::span<const std::byte> datagram, double nowSeconds);
		void Refuse();

		net::Transport *Transport_;

		Session Wire;
		Replica Replica_;
		Prediction Prediction_;

		ConnectorSettings Settings;

		// Lifetime is limited to the handshake.
		std::optional<net::Handshake> Exchange;

		std::array<std::byte, net::Handshake::MESSAGE_BYTES> Mine{};

		std::array<std::byte, net::Cookie::COOKIE_BYTES> Cookie_{};

		Stage Phase = Stage::Greeting;

		double SpokeAt = 0.0;
		bool Spoken = false;

		std::vector<std::byte> Datagram;

		// Whoever else is sharing this socket. See SetForeign.
		std::function<bool(std::span<const std::byte>, const net::Endpoint &)> Foreign;

		// Whoever is carrying something over this link. See OnUserMessage.
		std::function<void(std::span<const std::byte>)> UserMessages;

		Statistics Stats_;
	};
}
