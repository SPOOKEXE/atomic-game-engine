// A server and a client, over a real transport, with real framing.
//
// Everything in `Replication.cpp` hands byte vectors from one half to the
// other, which is the right way to test the protocol and says nothing about
// whether it survives a wire. This runs the same join and the same stream
// through `net`: a loopback `Transport`, a `Link` per side with its budgets and
// its acknowledgement window, `Packet::Write`/`Read` framing every datagram,
// and the reliable channel ordering what must not be reordered.
//
// That is `repo_layout.md` §16.6's claim made checkable — single-player rides
// the loopback with **real encoding**, so there is no configuration in which
// this path is skipped and no second lifecycle only a socket exercises.

#include <engine/core/Random.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/net/Transport.hpp>
#include <engine/replication/Authority.hpp>
#include <engine/replication/Replica.hpp>
#include <engine/replication/Session.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.replication.endtoend")

using engine::core::Name;
using engine::core::Random;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::net::ConnectionId;
using engine::net::MakeLoopbackTransport;
using engine::net::Transport;
using engine::replication::Authority;
using engine::replication::ClientId;
using engine::replication::Replica;
using engine::replication::Session;

namespace endtoend_test {
	struct Spot {
		float X = 0.0f;
		float Y = 0.0f;
	};

	void RegisterTypes() {
		static bool once = [] {
			engine::ecs::Components::Register<Spot>("endtoend_test.Spot");
			return true;
		}();
		(void)once;
	}

	// Two peers on a loopback, each with a session to the other.
	struct Wire {
		Wire() : Server("server"), Client("client") {
			RegisterTypes();

			Transports = MakeLoopbackTransport(2);
			REQUIRE(Transports.size() == 2);

			ServerSide =
				std::make_unique<Session>(*Transports[0], Transports[1]->Local(), ConnectionId{1, 1}, Now);
			ClientSide =
				std::make_unique<Session>(*Transports[1], Transports[0]->Local(), ConnectionId{2, 1}, Now);

			// Both ends live before anything is sent. A link still handshaking
			// refuses traffic, which is correct and is not what these cases are
			// about.
			REQUIRE(ServerSide->Link().CompleteHandshake(Now));
			REQUIRE(ClientSide->Link().CompleteHandshake(Now));

			Authority_.Replicate(Name("endtoend_test.Spot"));
			Server.Observe<Spot>();
		}

		// One full exchange: publish, send, carry, receive, apply, acknowledge.
		void Tick() {
			Now += 1.0 / 60.0;
			Tick_++;

			Authority_.Publish(Server, Tick_);
			for (const std::vector<std::byte> &message : Authority_.Outgoing(Handle)) {
				ServerSide->Send(message, Now);
			}
			Server.ClearChanges();

			ServerSide->Flush(Now);
			Carry(*Transports[1], *ClientSide);

			for (const std::vector<std::byte> &message : ClientSide->Inbound()) {
				Replica_.Receive(Client, message);
			}
			ClientSide->ClearInbound();

			const std::vector<std::byte> ack = Replica_.Acknowledge();
			if (!ack.empty()) {
				ClientSide->Send(ack, Now);
			}
			ClientSide->Flush(Now);
			Carry(*Transports[0], *ServerSide);

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
		void Carry(Transport &transport, Session &into) {
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

		Store Server;
		Store Client;
		std::vector<std::unique_ptr<Transport>> Transports;
		std::unique_ptr<Session> ServerSide;
		std::unique_ptr<Session> ClientSide;
		Authority Authority_;
		Replica Replica_;
		ClientId Handle = Authority_.Admit();
		double Now = 0.0;
		uint64_t Tick_ = 0;
	};
}

using namespace endtoend_test;

TEST_CASE("a client joins over a real transport", "[replication]") {
	Wire wire;

	std::vector<Entity> entities;
	for (int index = 0; index < 30; index++) {
		const Entity entity = wire.Server.Create();
		wire.Server.Set<Spot>(entity, Spot{static_cast<float>(index), 0.0f});
		entities.push_back(entity);
	}

	REQUIRE(wire.Join());

	for (int index = 0; index < 30; index++) {
		const Entity entity = entities[static_cast<size_t>(index)];
		REQUIRE(wire.Client.Alive(entity));
		REQUIRE(wire.Client.Get<Spot>(entity)->X == static_cast<float>(index));
	}
}

TEST_CASE("a snapshot crosses in chunks that each fit a datagram", "[replication]") {
	// The reason a snapshot is chunked at all. A world is megabytes and a
	// datagram is about twelve hundred bytes; a chunk that did not fit would be
	// refused by the transport and the client would never join.
	Wire wire;
	for (int index = 0; index < 600; index++) {
		wire.Server.Set<Spot>(wire.Server.Create(), Spot{static_cast<float>(index), 0.0f});
	}

	REQUIRE(wire.Join(1024));
	REQUIRE(wire.ServerSide->Stats().Undeliverable == 0);
	REQUIRE(wire.ServerSide->Stats().Refused == 0);
}

TEST_CASE("movement streams as deltas after the join", "[replication]") {
	Wire wire;

	std::vector<Entity> entities;
	for (int index = 0; index < 16; index++) {
		const Entity entity = wire.Server.Create();
		wire.Server.Set<Spot>(entity, Spot{0.0f, 0.0f});
		entities.push_back(entity);
	}
	REQUIRE(wire.Join());

	for (int round = 1; round <= 20; round++) {
		for (const Entity entity : entities) {
			wire.Server.GetMutable<Spot>(entity)->X = static_cast<float>(round);
		}
		wire.Tick();
	}

	for (const Entity entity : entities) {
		REQUIRE(wire.Client.Get<Spot>(entity)->X == 20.0f);
	}
	REQUIRE(wire.Replica_.Stats().Deltas > 0);
}

TEST_CASE("an input reaches the server over the reliable channel", "[replication]") {
	// Reliable because a lost input is a jump that never happened, and nothing
	// later covers it — unlike a delta, where the next one is already on its
	// way and is more correct.
	Wire wire;
	wire.Server.Set<Spot>(wire.Server.Create(), Spot{1.0f, 0.0f});
	REQUIRE(wire.Join());

	engine::core::ByteWriter writer;
	WriteMessage(writer, engine::replication::Input{wire.Tick_, {}});
	REQUIRE(wire.ClientSide->Send(writer.Bytes(), wire.Now));

	wire.Tick();
	REQUIRE(wire.Authority_.Inputs(wire.Handle).size() >= 1);
}

TEST_CASE("the acknowledgement gets back and the server sees it", "[replication]") {
	Wire wire;
	wire.Server.Set<Spot>(wire.Server.Create(), Spot{1.0f, 0.0f});
	REQUIRE(wire.Join());

	for (int round = 0; round < 4; round++) {
		wire.Tick();
	}

	// Not zero, and not ahead of what the client actually applied.
	const auto status = wire.Authority_.StatusOf(wire.Handle);
	REQUIRE(status.Applied > 0);
	REQUIRE(status.Applied <= wire.Replica_.Applied());
}

TEST_CASE("nothing malformed crosses a healthy link", "[replication]") {
	Wire wire;
	for (int index = 0; index < 40; index++) {
		wire.Server.Set<Spot>(wire.Server.Create(), Spot{static_cast<float>(index), 0.0f});
	}
	REQUIRE(wire.Join());

	for (int round = 0; round < 30; round++) {
		wire.Tick();
	}

	REQUIRE(wire.Replica_.Stats().Malformed == 0);
	REQUIRE(wire.Authority_.Stats().Refused == 0);
	REQUIRE(wire.ServerSide->Stats().Refused == 0);
	REQUIRE(wire.ClientSide->Stats().Refused == 0);
}
