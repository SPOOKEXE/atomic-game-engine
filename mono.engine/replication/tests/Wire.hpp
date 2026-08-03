#pragma once

// A server and a client, over a real transport, with real framing.
//
// The harness `EndToEnd.cpp` and `Loss.cpp` both drive. It lives in a header
// beside them rather than in either file because the second suite needs exactly
// the first one's loop with datagrams going missing in it, and two copies of a
// send loop is two places for a refusal to stop being handed back.
//
// Everything in `Replication.cpp` hands byte vectors from one half to the other,
// which is the right way to test the protocol and says nothing about whether it
// survives a wire. This runs the same join and the same stream through `net`: a
// loopback `Transport` per end, a `Link` per side with its budgets and its
// acknowledgement window, `Packet::Write`/`Read` framing every datagram, and the
// reliable channel ordering what must not be reordered.
//
// **Both ends are wrapped in a `net::LossyTransport`, and by default neither
// loses anything.** A harness with the wrapper only on the cases that use it
// would be two harnesses, and the one nobody was looking at would be the one
// that drifted.
//
// **Both sessions hold real keys, from a real `net::Handshake`.** A
// `Cipher::Sealer` has no constructor taking key material — that absence is the
// point of the type — so the only way a suite gets one is the way a connection
// does, and the consequence is that every case in these files runs over the
// encrypted stream rather than beside it.

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/net/Handshake.hpp>
#include <engine/net/LossyTransport.hpp>
#include <engine/net/Packet.hpp>
#include <engine/net/Transport.hpp>
#include <engine/replication/Authority.hpp>
#include <engine/replication/Replica.hpp>
#include <engine/replication/Session.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace replication_wire {

	// The one component these suites replicate.
	struct Spot {
		float X = 0.0f;
		float Y = 0.0f;
	};

	// A transport that watches the nonce counter of everything sent through it.
	//
	// **This is what makes "a nonce is never used twice" a checked property
	// rather than a sampled one.** One direction of one connection is one key
	// and one `net::Cipher::Sealer`, so "the counter on the wire strictly
	// increases in this direction" *is* "no nonce repeats under this key" — and
	// asserting it here applies it to every datagram every case in these files
	// produces, resends and all, rather than to whichever few a case remembered
	// to look at. A sampling test would pass over the one packet that repeated.
	//
	// It sits under the `LossyTransport` rather than over it, because loss is
	// applied on arrival and this has to see what was *sent*: a nonce that
	// repeated on a datagram the network then dropped has still been used twice
	// under that key, and the recording of it is what an attacker keeps.
	class Tap final : public engine::net::Transport {
	  public:
		// One datagram's framing, kept so a case can ask what went out.
		struct Record {
			engine::net::ChannelKind Channel = engine::net::ChannelKind::Unreliable;
			uint16_t Sequence = 0;
			uint64_t Counter = 0;
		};

		explicit Tap(std::unique_ptr<Transport> beneath) : Inner(std::move(beneath)) {}

		engine::net::TransportStatus
		Send(const engine::net::Endpoint &to, std::span<const std::byte> datagram) override {
			engine::core::ByteReader reader(datagram);
			const std::optional<engine::net::Packet::Inbound> packet = engine::net::Packet::Read(reader);

			// The handshake channel is the one that has no keys yet and carries
			// no counter, so it is skipped rather than counted as a repeat of
			// zero.
			if (packet.has_value() && packet->Header.Channel != engine::net::ChannelKind::Handshake) {
				// Strictly greater, not merely different. A counter that went
				// backwards is one a later frame can reach again.
				REQUIRE((History.empty() || packet->Header.Counter > History.back().Counter));

				History.push_back(
					Record{packet->Header.Channel, packet->Header.Sequence, packet->Header.Counter}
				);
				Latest.assign(datagram.begin(), datagram.end());
			}

			return Inner->Send(to, datagram);
		}

		Inbound Receive(std::vector<std::byte> &datagram) override {
			return Inner->Receive(datagram);
		}

		engine::net::Endpoint Local() const override {
			return Inner->Local();
		}

		bool Open() const override {
			return Inner->Open();
		}

		void Close() override {
			Inner->Close();
		}

		// The last sealed datagram that went out, exactly as the wire saw it.
		const std::vector<std::byte> &Last() const {
			return Latest;
		}

		// Every sealed datagram's framing, in the order they went.
		std::span<const Record> Sent() const {
			return History;
		}

	  private:
		std::unique_ptr<Transport> Inner;
		std::vector<Record> History;
		std::vector<std::byte> Latest;
	};

	// Runs one X25519 exchange and gives each session its half.
	//
	// The same two calls `Listener` and `Connector` make, in the same order.
	inline void Agree(engine::replication::Session &server, engine::replication::Session &client) {
		std::optional<engine::net::Handshake> responder =
			engine::net::Handshake::Begin(engine::net::HandshakeRole::Responder);
		std::optional<engine::net::Handshake> initiator =
			engine::net::Handshake::Begin(engine::net::HandshakeRole::Initiator);
		REQUIRE(responder.has_value());
		REQUIRE(initiator.has_value());

		REQUIRE(responder->Consume(initiator->Message()));
		REQUIRE(initiator->Consume(responder->Message()));

		std::optional<engine::net::Handshake::Session> serving = responder->TakeKeys();
		std::optional<engine::net::Handshake::Session> replying = initiator->TakeKeys();
		REQUIRE(serving.has_value());
		REQUIRE(replying.has_value());

		REQUIRE(server.AdoptKeys(std::move(*serving)));
		REQUIRE(client.AdoptKeys(std::move(*replying)));
	}

	inline void RegisterTypes() {
		static bool once = [] {
			engine::ecs::Components::Register<Spot>("endtoend_test.Spot");
			return true;
		}();
		(void)once;
	}

	// Two peers on a loopback, each with a session to the other.
	struct Wire {
		explicit Wire(
			const engine::replication::SessionSettings &session = {},
			const engine::replication::AuthoritySettings &authority = {},
			const engine::net::LossSettings &toClient = {},
			const engine::net::LossSettings &toServer = {}
		)
			: Server("server"), Client("client"), Authority_(authority) {
			RegisterTypes();

			std::vector<std::unique_ptr<engine::net::Transport>> ends = engine::net::MakeLoopbackTransport(2);
			REQUIRE(ends.size() == 2);

			// Loss is applied where a datagram lands, so the wrapper on the
			// client's end is the one that loses what the server sent. The tap
			// goes underneath both, because it has to see what was sent rather
			// than what survived.
			std::unique_ptr<Tap> serverTap = std::make_unique<Tap>(std::move(ends[0]));
			std::unique_ptr<Tap> clientTap = std::make_unique<Tap>(std::move(ends[1]));
			ServerTap = serverTap.get();
			ClientTap = clientTap.get();

			ServerEnd = std::make_unique<engine::net::LossyTransport>(std::move(serverTap), toServer);
			ClientEnd = std::make_unique<engine::net::LossyTransport>(std::move(clientTap), toClient);

			ServerSide = std::make_unique<engine::replication::Session>(
				*ServerEnd, ClientEnd->Local(), engine::net::ConnectionId{1, 1}, Now, session
			);
			ClientSide = std::make_unique<engine::replication::Session>(
				*ClientEnd, ServerEnd->Local(), engine::net::ConnectionId{2, 1}, Now, session
			);

			// Both ends live before anything is sent. A link still handshaking
			// refuses traffic, which is correct and is not what these cases are
			// about.
			REQUIRE(ServerSide->Link().CompleteHandshake(Now));
			REQUIRE(ClientSide->Link().CompleteHandshake(Now));

			// And both hold keys, because a session that does not carries
			// nothing at all.
			Agree(*ServerSide, *ClientSide);

			Authority_.Replicate(engine::core::Name("endtoend_test.Spot"));
			Server.Observe<Spot>();
		}

		// One full exchange: publish, send, carry, receive, apply, acknowledge.
		void Tick() {
			Now += 1.0 / 60.0;
			Tick_++;

			Authority_.Publish(Server, Tick_);

			// Refusals handed straight back, exactly as `Listener` does. **Not
			// a convenience of the harness**: a snapshot chunk the link refuses
			// is a permanent hole unless the authority is told, so a suite that
			// ignored the return value would be exercising a send loop no
			// program uses and would pass while the real one hung.
			const std::span<const std::vector<std::byte>> messages = Authority_.Outgoing(Handle);
			for (size_t index = 0; index < messages.size(); index++) {
				if (!ServerSide->Send(messages[index], Now)) {
					Authority_.Unsent(Handle, index);
				}
			}
			Server.ClearChanges();

			ServerSide->Flush(Now);
			Carry(*ClientEnd, *ClientSide);

			for (const std::vector<std::byte> &message : ClientSide->Inbound()) {
				Replica_.Receive(Client, message);
			}
			ClientSide->ClearInbound();

			const std::vector<std::byte> ack = Replica_.Acknowledge();
			if (!ack.empty()) {
				ClientSide->Send(ack, Now);
			}
			ClientSide->Flush(Now);
			Carry(*ServerEnd, *ServerSide);

			for (const std::vector<std::byte> &message : ServerSide->Inbound()) {
				Authority_.Receive(Handle, message);
			}
			ServerSide->ClearInbound();

			// Both links advance, so an idle timeout is a thing these cases
			// could hit rather than a thing they are exempt from.
			ServerSide->Link().Advance(Now);
			ClientSide->Link().Advance(Now);
			ServerSide->Link().ResetBudget();
			ClientSide->Link().ResetBudget();
		}

		// Drains one transport into its session.
		void Carry(engine::net::Transport &transport, engine::replication::Session &into) {
			std::vector<std::byte> datagram;
			while (transport.Receive(datagram).Status == engine::net::TransportStatus::Ok) {
				into.Receive(datagram, Now);
			}
		}

		bool Join(int limit = 256) {
			for (int attempt = 0; attempt < limit && !Replica_.Joined(); attempt++) {
				Tick();
			}
			return Replica_.Joined();
		}

		engine::ecs::Store Server;
		engine::ecs::Store Client;

		// The wrapper on each end. `ClientEnd` loses what the server sent.
		std::unique_ptr<engine::net::LossyTransport> ServerEnd;
		std::unique_ptr<engine::net::LossyTransport> ClientEnd;

		// Owned by the wrappers above; borrowed here so a case can ask what
		// actually went out. `ServerTap` sees what the server sent.
		Tap *ServerTap = nullptr;
		Tap *ClientTap = nullptr;

		std::unique_ptr<engine::replication::Session> ServerSide;
		std::unique_ptr<engine::replication::Session> ClientSide;
		engine::replication::Authority Authority_;
		engine::replication::Replica Replica_;
		engine::replication::ClientId Handle = Authority_.Admit();
		double Now = 0.0;
		uint64_t Tick_ = 0;
	};
}
