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

		SessionSettings Session;

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
			uint64_t Refused = 0;

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

		Statistics Stats_;
	};
}
