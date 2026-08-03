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
#include <engine/replication/Connector.hpp>
#include <engine/replication/Listener.hpp>
#include <engine/replication/Replica.hpp>
#include <engine/replication/Session.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <span>
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
		explicit Wire(
			const engine::replication::SessionSettings &session = {},
			const engine::replication::AuthoritySettings &authority = {}
		)
			: Server("server"), Client("client"), Authority_(authority) {
			RegisterTypes();

			Transports = MakeLoopbackTransport(2);
			REQUIRE(Transports.size() == 2);

			ServerSide = std::make_unique<Session>(
				*Transports[0], Transports[1]->Local(), ConnectionId{1, 1}, Now, session
			);
			ClientSide = std::make_unique<Session>(
				*Transports[1], Transports[0]->Local(), ConnectionId{2, 1}, Now, session
			);

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

TEST_CASE("a snapshot chunk the link refuses is sent again", "[replication]") {
	// **The regression, stated rather than waited for.** `net`'s budgets are
	// per tick and are numbers a suite sets, so "the link will carry four
	// packets and the authority wants to send eight" is a fact this case
	// declares — no load, no timing, no flake. A world of two hundred entities
	// is about two hundred chunks, and half of every tick's eight were refused.
	//
	// Before the fix the cursor moved when a chunk was *built*, so a refused
	// chunk was a hole nothing ever filled: the client applied almost all of
	// the snapshot, never reached the last byte, never joined, and then refused
	// every delta that followed as stale. The counts from the server suite were
	// 184 chunks applied against 17865 refusals, which reads like a protocol
	// error and was a cursor.
	engine::replication::SessionSettings session;
	session.Link.PacketsPerTick = 4;

	engine::replication::AuthoritySettings authority;
	authority.ChunksPerTick = 8;

	Wire wire(session, authority);
	for (int index = 0; index < 200; index++) {
		wire.Server.Set<Spot>(wire.Server.Create(), Spot{static_cast<float>(index), 0.0f});
	}

	REQUIRE(wire.Join(512));

	// The budget really was exceeded, so this case is testing the recovery
	// rather than a link that happened to have room. If this is ever zero the
	// case above proves nothing.
	REQUIRE(wire.ServerSide->Link().Stats().SendsOverBudget > 0);

	// And the world that arrived is the whole one. A snapshot reassembled from
	// chunks that were re-sent must not differ from one that went first time.
	REQUIRE(wire.Client.CountMatching<Spot>() == 200);
}

TEST_CASE("the listener's own send loop hands refusals back", "[replication]") {
	// The case above drives `Authority` and `Session` by hand, which is the
	// right shape for a protocol suite and proves nothing about the loop a
	// program actually runs. **`Listener::Publish` is where the return value of
	// `Send` used to be dropped on the floor**, and dropping it there is what
	// turned a refused chunk into a client that never joined — so this stands up
	// the real `Listener` and the real `Connector` over a loopback and squeezes
	// the same budget.
	engine::replication::ListenerSettings serving;
	serving.Session.Link.PacketsPerTick = 4;
	serving.Authority.ChunksPerTick = 8;

	engine::replication::ConnectorSettings connecting;

	endtoend_test::RegisterTypes();

	std::vector<std::unique_ptr<Transport>> transports = MakeLoopbackTransport(2);
	REQUIRE(transports.size() == 2);

	Store world("server");
	world.Observe<Spot>();
	for (int index = 0; index < 200; index++) {
		world.Set<Spot>(world.Create(), Spot{static_cast<float>(index), 0.0f});
	}

	double now = 0.0;
	engine::replication::Listener listener(*transports[0], serving);
	listener.Authority().Replicate(Name("endtoend_test.Spot"));

	Store replica("client");
	engine::replication::Connector connector(*transports[1], transports[0]->Local(), now, connecting);

	for (int tick = 1; tick <= 512 && !connector.Joined(); tick++) {
		now += 1.0 / 60.0;

		connector.Poll(replica, now);
		listener.Poll(now);
		listener.Publish(world, static_cast<uint64_t>(tick), now);
		listener.Advance(now);
		connector.Poll(replica, now);
		connector.Advance(now);
	}

	REQUIRE(connector.Joined());
	REQUIRE(replica.CountMatching<Spot>() == 200);
}

TEST_CASE("a creation the link refused is announced again", "[replication]") {
	// **A creation is said exactly once, and the known set moves when it is
	// built.** So a delta message carrying creations that the link would not
	// take used to leave the server believing a client had been told about
	// entities it had never heard of — and every component value for them after
	// that is a value for a row the replica does not hold, which `Replica` drops
	// without a word. There is no re-snapshot either: the client is
	// acknowledging happily and is not behind.
	//
	// Three hundred entities appearing at once against a link that will carry
	// two packets a tick is that case, stated rather than waited for.
	engine::replication::SessionSettings session;
	session.Link.PacketsPerTick = 2;

	Wire wire(session);
	wire.Server.Set<Spot>(wire.Server.Create(), Spot{0.0f, 0.0f});
	REQUIRE(wire.Join(256));

	std::vector<Entity> late;
	for (int index = 0; index < 300; index++) {
		const Entity entity = wire.Server.Create();
		wire.Server.Set<Spot>(entity, Spot{static_cast<float>(index), 1.0f});
		late.push_back(entity);
	}

	size_t restarts = 0;
	for (int tick = 0; tick < 400; tick++) {
		wire.Tick();
		restarts += wire.Authority_.Stats().Resnapshots;
	}

	REQUIRE(wire.ServerSide->Link().Stats().SendsOverBudget > 0);
	for (const Entity entity : late) {
		REQUIRE(wire.Client.Alive(entity));
	}

	// **And by being told again, not by being sent the world again.** A client
	// holding entities the server thinks it announced falls behind and is
	// eventually re-snapshotted, which does repair it — two seconds later and
	// at the cost of the whole world twice over. A repair that expensive
	// looks like a working system from every angle except the bandwidth graph,
	// which is why this is asserted rather than left to the count above.
	REQUIRE(restarts == 0);
}

TEST_CASE("the refusal counter moves only when the link is really over", "[replication]") {
	// The other half of the case above, and the reason it is a separate one:
	// `SendsOverBudget` is what `docs/DEFERRED.md` calls the reopen signal, so
	// a build that moved it on an ordinary tick would make the signal useless.
	// Under the default budgets a small world never touches it.
	Wire wire;

	std::vector<Entity> entities;
	for (int index = 0; index < 40; index++) {
		const Entity entity = wire.Server.Create();
		wire.Server.Set<Spot>(entity, Spot{static_cast<float>(index), 0.0f});
		entities.push_back(entity);
	}
	REQUIRE(wire.Join());

	for (int round = 0; round < 30; round++) {
		for (const Entity entity : entities) {
			wire.Server.GetMutable<Spot>(entity)->X = static_cast<float>(round);
		}
		wire.Tick();
	}

	REQUIRE(wire.ServerSide->Link().Stats().SendsOverBudget == 0);
	REQUIRE(wire.Authority_.Stats().Deferred == 0);
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
