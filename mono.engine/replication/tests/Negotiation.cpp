// Which stack a connection ends up on, and who decided.
//
// **The rule under test is that the server decides and the client has no
// vote.** A connector opens with QUIC whatever it was configured with; a
// listener that does not serve QUIC says so in one datagram; and the fallback
// that follows is the client's own, bounded, and logged.
//
// The three modes are `net::WireMode`'s three, and each is checked from the
// outside - what a peer that speaks the wrong stack gets back, and whether it
// then joins - rather than by reading a flag off either end.

#include "Wire.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/net/LossyTransport.hpp>
#include <engine/net/Packet.hpp>
#include <engine/net/Transport.hpp>
#include <engine/net/Wire.hpp>
#include <engine/net/quic/Tls.hpp>
#include <engine/replication/Admission.hpp>
#include <engine/replication/Connector.hpp>
#include <engine/replication/Listener.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/WeatherClock.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

TEST_SUITE_ID("engine.replication.negotiation")
TEST_DEPENDS("engine.replication.quicwire")
TEST_DEPENDS("engine.net.wire")

using engine::core::Name;
using engine::net::LossSettings;
using engine::net::LossyTransport;
using engine::net::MakeLoopbackTransport;
using engine::net::Transport;
using engine::net::WireKind;
using engine::net::WireMode;
using engine::replication::Admission;
using engine::replication::AdmissionKind;
using engine::replication::Connector;
using engine::replication::ConnectorSettings;
using engine::replication::Listener;
using engine::replication::ListenerSettings;

using namespace replication_wire;

namespace {
	// The seed a listener's identity is derived from. Stated rather than drawn,
	// so a failure reproduces from the file alone.
	std::array<std::byte, 32> Seed() {
		std::array<std::byte, 32> seed{};
		for (size_t index = 0; index < seed.size(); index++) {
			seed[index] = static_cast<std::byte>(index * 5 + 17);
		}
		return seed;
	}

	// A listener on one side of a loopback pair, and whatever connectors a case
	// puts on the other.
	//
	// **Neither end is pinned**, deliberately: what is under test is the
	// transport decision and not the identity check, and a pin that had to be
	// installed differently per wire would be a second thing each case gets
	// wrong.
	struct Field {
		std::vector<std::unique_ptr<Transport>> Ends;
		std::unique_ptr<Transport> ServerWire;
		std::unique_ptr<Transport> ClientWire;
		engine::ecs::Store World{"server"};
		engine::ecs::Store Replica{"client"};
		std::unique_ptr<Listener> Server;
		std::unique_ptr<Connector> Client;
		double Now = 0.0;

		explicit Field(WireMode mode, const LossSettings &clientLoss = {}) {
			RegisterTypes();
			Ends = MakeLoopbackTransport(2);
			REQUIRE(Ends.size() == 2);

			// Both ends wrapped whether or not they lose anything, for
			// `Wire.hpp`'s reason: a harness with the wrapper only on the cases
			// that use it is two harnesses.
			ServerWire = std::make_unique<LossyTransport>(std::move(Ends[0]));
			ClientWire = std::make_unique<LossyTransport>(std::move(Ends[1]), clientLoss);

			ListenerSettings serving;
			serving.Wire = mode;
			serving.Quic.Connection.Tls.Seed = Seed();
			serving.Quic.Connection.Tls.HasSeed = true;
			Server = std::make_unique<Listener>(*ServerWire, serving);
			Server->Authority().Replicate(Name("endtoend_test.Spot"));
		}

		void Connect(const ConnectorSettings &settings) {
			Client = std::make_unique<Connector>(*ClientWire, ServerWire->Local(), Now, settings);
		}

		// One barrier's worth, in the order a program runs them.
		void Tick(uint64_t tick) {
			Now += 1.0 / 60.0;
			if (Client != nullptr) {
				Client->Poll(Replica, Now);
			}
			Server->Poll(Now);
			Server->Publish(World, tick, Now);
			Server->Advance(Now);
			if (Client != nullptr) {
				Client->Poll(Replica, Now);
				Client->Advance(Now);
			}
		}

		bool Admit(uint64_t ticks = 400) {
			for (uint64_t tick = 1; tick <= ticks; ++tick) {
				if (Client->Admitted() || Client->Rejected()) {
					break;
				}
				Tick(tick);
			}
			// **Two more, because the two ends do not finish on the same tick.**
			// A TLS client is done when it has verified the server's Finished; a
			// server is done when the client's arrives, which is one flight
			// later.
			Tick(ticks + 1);
			Tick(ticks + 2);
			return Client->Admitted();
		}
	};

	// The first datagram a client on the old stack sends, framed as that stack
	// frames it.
	std::vector<std::byte> DatagramHello() {
		engine::replication::Hello hello;
		hello.PublicKey.fill(std::byte{0x11});

		engine::core::ByteWriter body;
		WriteAdmission(body, hello);

		engine::core::ByteWriter datagram;
		REQUIRE(engine::replication::FrameAdmission(datagram, body.Bytes()));
		return {datagram.Bytes().begin(), datagram.Bytes().end()};
	}

	// Whatever the listener sent back, parsed as an admission message.
	std::optional<Admission> AnswerTo(Field &field, const std::vector<std::byte> &sent) {
		field.ClientWire->Send(field.ServerWire->Local(), sent);
		field.Now += 1.0 / 60.0;
		field.Server->Poll(field.Now);

		std::vector<std::byte> reply;
		if (field.ClientWire->Receive(reply).Status != engine::net::TransportStatus::Ok) {
			return std::nullopt;
		}

		engine::core::ByteReader reader(reply);
		const std::optional<engine::net::Packet::Inbound> packet = engine::net::Packet::Read(reader);
		if (!packet.has_value()) {
			return std::nullopt;
		}

		engine::core::ByteReader body(packet->Payload);
		Admission message;
		if (!ReadAdmission(body, message)) {
			return std::nullopt;
		}
		return message;
	}
}

// --- quic only --------------------------------------------------------------

TEST_CASE("a quic-only server admits a quic client", "[replication][negotiation]") {
	Field field(WireMode::Quic);
	field.Connect({});
	REQUIRE(field.Admit());

	// **One attempt, because the first one is QUIC and there was nothing to
	// fall back from.**
	CHECK(field.Client->Wire() == WireKind::Quic);
	CHECK(field.Client->Attempts() == 1);
	CHECK(field.Server->Count() == 1);
	CHECK(field.Server->Stats().Mismatched == 0);
}

TEST_CASE("a quic-only server refuses a datagram hello by name", "[replication][negotiation]") {
	Field field(WireMode::Quic);
	const std::vector<std::byte> hello = DatagramHello();

	const std::optional<Admission> answer = AnswerTo(field, hello);
	REQUIRE(answer.has_value());
	CHECK(answer->Kind == AdmissionKind::Refuse);

	// Which wire it does serve, so a client tries the right one next rather
	// than the next one in a list.
	CHECK(answer->Refusal.Wire == WireKind::Quic);

	// Counted apart from `Refused`: this datagram was a perfectly good opening
	// message for the other stack, which is a different fix from a port
	// somebody is probing.
	CHECK(field.Server->Stats().Mismatched == 1);
	CHECK(field.Server->Stats().Refused == 0);
	CHECK(field.Server->Count() == 0);
}

TEST_CASE("a refusal is smaller than the hello that caused it", "[replication][negotiation]") {
	// `net/AGENTS.md`: a responder that answers a 35-byte hello with something
	// larger is a reflector somebody else's traffic can be bounced off, and the
	// amplification factor is the whole of what makes that worth doing.
	Field field(WireMode::Quic);
	const std::vector<std::byte> hello = DatagramHello();

	field.ClientWire->Send(field.ServerWire->Local(), hello);
	field.Server->Poll(field.Now);

	std::vector<std::byte> reply;
	REQUIRE(field.ClientWire->Receive(reply).Status == engine::net::TransportStatus::Ok);
	CHECK(reply.size() < hello.size());
}

// --- datagram only ----------------------------------------------------------

TEST_CASE("a datagram-only server refuses quic and the client joins anyway", "[replication][negotiation]") {
	Field field(WireMode::Datagram);
	field.Connect({});
	REQUIRE(field.Admit());

	// **The whole point of the refusal**: the client opened with QUIC, was told
	// no in one round trip, and joined on the other stack without anybody
	// configuring it.
	CHECK(field.Client->Wire() == WireKind::Datagram);
	CHECK(field.Client->Attempts() == 2);
	CHECK(field.Server->Count() == 1);
	CHECK(field.Server->Stats().Mismatched >= 1);
}

TEST_CASE("the refusal is what makes the fallback fast", "[replication][negotiation]") {
	// A deadline far longer than this case runs for. If the fallback waited for
	// it rather than acting on the refusal, nothing here would join.
	ConnectorSettings connecting;
	connecting.AttemptSeconds = 1000.0;

	Field field(WireMode::Datagram);
	field.Connect(connecting);
	REQUIRE(field.Admit(120));
	CHECK(field.Client->Wire() == WireKind::Datagram);
}

// --- both -------------------------------------------------------------------

TEST_CASE("a server serving both takes whichever arrives", "[replication][negotiation]") {
	Field field(WireMode::Both);

	// The one that opens with QUIC, which is every client with nothing to say
	// otherwise.
	field.Connect({});
	REQUIRE(field.Admit());
	CHECK(field.Client->Wire() == WireKind::Quic);

	// And one that heard a datagram-only advert and opened there. A server on
	// `Both` takes it without refusing anything.
	ConnectorSettings old;
	old.Advertised = WireMode::Datagram;
	Connector second(*field.ClientWire, field.ServerWire->Local(), field.Now, old);

	for (uint64_t tick = 1; tick <= 200 && !second.Admitted(); ++tick) {
		field.Now += 1.0 / 60.0;
		second.Poll(field.Replica, field.Now);
		field.Server->Poll(field.Now);
		field.Server->Advance(field.Now);
		second.Advance(field.Now);
	}

	CHECK(second.Admitted());
	CHECK(second.Wire() == WireKind::Datagram);
	CHECK(second.Attempts() == 1);
	CHECK(field.Server->Count() == 2);
	CHECK(field.Server->Stats().Mismatched == 0);
}

// --- what an advert saves ---------------------------------------------------

TEST_CASE("an advertised transport spares the refusal entirely", "[replication][negotiation]") {
	ConnectorSettings connecting;
	connecting.Advertised = WireMode::Datagram;

	Field field(WireMode::Datagram);
	field.Connect(connecting);
	REQUIRE(field.Admit());

	// One attempt and no refusal at all, where the same client with no advert
	// pays one of each.
	CHECK(field.Client->Wire() == WireKind::Datagram);
	CHECK(field.Client->Attempts() == 1);
	CHECK(field.Server->Stats().Mismatched == 0);
}

TEST_CASE("an advert that says quic-only stops the client falling back", "[replication][negotiation]") {
	// Nothing is listening, so the QUIC attempt times out. A client told the
	// server serves only QUIC has nothing to fall back *to*, so it gives up
	// rather than spending a second deadline on a stack the server refuses.
	std::vector<std::unique_ptr<Transport>> ends = MakeLoopbackTransport(2);
	REQUIRE(ends.size() == 2);

	ConnectorSettings connecting;
	connecting.Advertised = WireMode::Quic;
	connecting.AttemptSeconds = 0.1;

	engine::ecs::Store replica("client");
	Connector client(*ends[1], ends[0]->Local(), 0.0, connecting);

	double now = 0.0;
	for (int tick = 0; tick < 60 && !client.Rejected(); tick++) {
		now += 1.0 / 60.0;
		client.Poll(replica, now);
		client.Advance(now);
	}

	CHECK(client.Rejected());
	CHECK(client.Attempts() == 1);
}

TEST_CASE("a silent server costs two attempts and no more", "[replication][negotiation]") {
	std::vector<std::unique_ptr<Transport>> ends = MakeLoopbackTransport(2);
	REQUIRE(ends.size() == 2);

	ConnectorSettings connecting;
	connecting.AttemptSeconds = 0.1;

	engine::ecs::Store replica("client");
	Connector client(*ends[1], ends[0]->Local(), 0.0, connecting);

	double now = 0.0;
	for (int tick = 0; tick < 120 && !client.Rejected(); tick++) {
		now += 1.0 / 60.0;
		client.Poll(replica, now);
		client.Advance(now);
	}

	// **Bounded, and the bound is two because there are two stacks.** A client
	// that kept cycling would be one that never reports a wrong address.
	CHECK(client.Rejected());
	CHECK(client.Attempts() == Connector::MAXIMUM_ATTEMPTS);
	CHECK(client.Wire() == WireKind::Datagram);
}

// --- over a link that loses things ------------------------------------------

TEST_CASE("a lost refusal costs the deadline and not the connection", "[replication][negotiation]") {
	// **The case the timeout exists for.** The refusal is one datagram and one
	// datagram can go missing, so a client that only ever fell back on an
	// explicit refusal would hang for ever on a link that dropped it. Nominated
	// by ordinal rather than by percentage: arrivals are counted from zero and
	// the first thing this client hears back is the refusal, so arrival zero is
	// exactly that datagram and nothing else.
	LossSettings lost;
	lost.Drop = {0};

	ConnectorSettings connecting;
	connecting.AttemptSeconds = 0.25;

	Field field(WireMode::Datagram, lost);
	field.Connect(connecting);
	REQUIRE(field.Admit());

	CHECK(field.Client->Wire() == WireKind::Datagram);
	CHECK(field.Client->Attempts() == 2);
}

TEST_CASE("the fallback survives a link losing the handshake too", "[replication][negotiation]") {
	// The refusal and the first two answers the datagram exchange gets back.
	// What is being checked is that a fallback is an ordinary connection once it
	// has started, repaired by the retransmission that stack already has -
	// `ConnectorSettings::RepeatEverySeconds` is what drives it.
	LossSettings lost;
	lost.Drop = {0, 1, 2};

	ConnectorSettings connecting;
	connecting.AttemptSeconds = 1.5;

	Field field(WireMode::Datagram, lost);
	field.Connect(connecting);
	REQUIRE(field.Admit());
	CHECK(field.Client->Wire() == WireKind::Datagram);
}

TEST_CASE("a quic server still admits over a link that loses the initial", "[replication][negotiation]") {
	LossSettings lost;
	lost.Drop = {1, 3};

	ConnectorSettings connecting;
	// Long enough that the QUIC handshake's own retransmission is what repairs
	// this, rather than the fallback quietly rescuing the case.
	connecting.AttemptSeconds = 30.0;

	Field field(WireMode::Quic, lost);
	field.Connect(connecting);
	REQUIRE(field.Admit());

	CHECK(field.Client->Wire() == WireKind::Quic);
	CHECK(field.Client->Attempts() == 1);
}

TEST_CASE(
	"weather clock joins and updates on either wire while local elapsed time is preserved",
	"[replication][negotiation][weather]"
) {
	const auto mode = GENERATE(WireMode::Datagram, WireMode::Quic);
	engine::scene::RegisterSceneComponents();
	Field field(mode);
	const auto provider = field.World.Create();
	engine::scene::WeatherClock clock;
	clock.Tick = 4000;
	clock.ElapsedSeconds = 200;
	clock.Sequence = 1;
	clock.PhaseTick = 80;
	clock.DisplacementQ8 = {400, 0, 100};
	clock.Paused = true;
	field.World.Set(provider, clock);
	field.Server->Authority().Replicate(
		Name("scene.WeatherClock"), engine::replication::ChangeDetection::Observed
	);
	field.Replica.AdvanceTick(0.125f);
	const auto before = field.Replica.Time();
	ConnectorSettings settings;
	settings.AttemptSeconds = 2;
	field.Connect(settings);
	uint64_t observations = 0;
	field.Client->OnTiming([&](engine::ecs::Store &store, double nowSeconds, double roundTripSeconds) {
		CHECK(nowSeconds == field.Now);
		CHECK(roundTripSeconds >= 0);
		engine::scene::SynchronizeWeatherClocks(store, nowSeconds, roundTripSeconds);
		++observations;
	});
	field.Client->OnUserState(engine::scene::AcceptWeatherClockUpdate);
	for (uint64_t tick = 1; tick < 400 && !field.Client->Joined(); ++tick)
		field.Tick(tick);
	REQUIRE(field.Client->Joined());
	REQUIRE(field.Replica.Get<engine::scene::WeatherClock>(provider));
	CHECK(engine::scene::WeatherClockOf(field.Replica, provider).Tick == 4000);
	CHECK(engine::scene::WeatherClockOf(field.Replica, provider).ElapsedSeconds == 200);
	CHECK(engine::scene::WeatherClockOf(field.Replica, provider).DisplacementQ8 == clock.DisplacementQ8);
	CHECK(field.Replica.Time().Elapsed == before.Elapsed);
	CHECK(field.Replica.Time().Tick == before.Tick);
	const auto arrived = observations;
	field.Client->Poll(field.Replica, field.Now);
	CHECK(observations == arrived + 1);
	clock.Sequence = 2;
	clock.Tick = 5000;
	clock.ElapsedSeconds = 250;
	clock.DisplacementQ8 = {1200, 0, 100};
	field.World.Set(provider, clock);
	for (uint64_t tick = 500; tick < 520; ++tick)
		field.Tick(tick);
	CHECK(engine::scene::WeatherClockOf(field.Replica, provider).Tick == 5000);
	CHECK(engine::scene::WeatherClockOf(field.Replica, provider).ElapsedSeconds == 250);
	CHECK(engine::scene::WeatherClockOf(field.Replica, provider).DisplacementQ8 == clock.DisplacementQ8);
	CHECK(field.Replica.Time().Elapsed == before.Elapsed);
	clock.Sequence = 3;
	clock.Tick = 6000;
	clock.ElapsedSeconds = 300;
	clock.WorldPaused = true;
	engine::core::ByteWriter finalAnchor;
	engine::scene::WriteWeatherClockUpdate(finalAnchor, provider, clock);
	bool visible = false;
	field.Server->Authority().SetInterestBatch([&](engine::replication::ClientId,
												   const engine::ecs::Store &,
												   std::span<const engine::ecs::Entity> candidates,
												   std::span<uint8_t> accepted) {
		for (size_t index = 0; index < candidates.size(); ++index)
			accepted[index] = visible ? 1 : 0;
	});
	REQUIRE(field.Server->BroadcastEntityState(field.World, provider, finalAnchor.Bytes(), field.Now));
	field.Now += 0.02;
	field.Server->Advance(field.Now);
	field.Client->Poll(field.Replica, field.Now);
	CHECK(engine::scene::WeatherClockOf(field.Replica, provider).Tick == 5000);
	visible = true;
	REQUIRE(field.Server->BroadcastEntityState(field.World, provider, finalAnchor.Bytes(), field.Now));
	// No world tick is advanced or newly acknowledged for this suspension anchor.
	for (int pump = 0; pump < 5; ++pump) {
		field.Now += 0.02;
		field.Server->Advance(field.Now);
		field.Client->Poll(field.Replica, field.Now);
		field.Client->Advance(field.Now);
	}
	CHECK(engine::scene::WeatherClockOf(field.Replica, provider).Tick == 6000);
	CHECK(engine::scene::WeatherClockOf(field.Replica, provider).ElapsedSeconds == 300);
	CHECK(engine::scene::WeatherClockOf(field.Replica, provider).WorldPaused);
	CHECK(field.Replica.Time().Elapsed == before.Elapsed);
}

TEST_CASE(
	"reliable pause anchors survive an unfinished joining snapshot on either wire",
	"[replication][negotiation][weather]"
) {
	const auto mode = GENERATE(WireMode::Datagram, WireMode::Quic);
	const bool providerPresent = GENERATE(false, true);
	engine::scene::RegisterSceneComponents();
	Field field(mode);
	const auto provider = field.World.Create();
	engine::scene::WeatherClock beforePause;
	beforePause.Tick = 70;
	beforePause.ElapsedSeconds = 3;
	beforePause.ElapsedFractionQ32 = uint32_t{1} << 31;
	beforePause.Sequence = 1;
	field.World.Set(provider, beforePause);
	if (providerPresent) {
		REQUIRE(field.Replica.CreateAt(provider));
		field.Replica.Set(provider, beforePause);
	}
	const auto bulk = field.World.Create();
	field.World.Set(bulk, Bulk{std::string(24 * 1024, 'w')});
	field.Server->Authority().Replicate(Name("endtoend_test.Bulk"));
	field.Server->Authority().Replicate(Name("scene.WeatherClock"));
	ConnectorSettings settings;
	settings.AttemptSeconds = 2;
	field.Connect(settings);
	field.Client->OnUserState(engine::scene::AcceptWeatherClockUpdate);
	field.Client->OnTiming(engine::scene::SynchronizeWeatherClocks);
	// Complete admission without publishing any world state.
	for (int pump = 0; pump < 200; ++pump) {
		field.Now += 1.0 / 60.0;
		field.Client->Poll(field.Replica, field.Now);
		field.Server->Poll(field.Now);
		field.Server->Advance(field.Now);
		field.Client->Poll(field.Replica, field.Now);
		field.Client->Advance(field.Now);
		if (field.Client->Admitted() && field.Server->Carrying() != 0) break;
	}
	REQUIRE(field.Client->Admitted());
	REQUIRE(field.Server->Carrying() == 1);
	REQUIRE_FALSE(field.Client->Joined());
	// Capture the active snapshot, then suspend while that snapshot is in flight.
	field.Server->Publish(field.World, 1, field.Now);
	auto paused = beforePause;
	REQUIRE(engine::scene::AdvanceWeatherElapsed(paused, 1));
	paused.Sequence = 2;
	paused.Tick = 71;
	paused.WorldPaused = true;
	field.World.Set(provider, paused);
	engine::core::ByteWriter anchor;
	engine::scene::WriteWeatherClockUpdate(anchor, provider, paused);
	REQUIRE(field.Server->BroadcastEntityState(field.World, provider, anchor.Bytes(), field.Now));
	for (int pump = 0; pump < 40; ++pump) {
		field.Now += 1.0 / 60.0;
		field.Server->Advance(field.Now);
		field.Client->Poll(field.Replica, field.Now);
		field.Client->Advance(field.Now);
		field.Server->Poll(field.Now);
		const auto *inbox = field.Replica.Resource<engine::scene::WeatherClockInbox>();
		if (inbox != nullptr && inbox->Count == 1) break;
	}
	REQUIRE_FALSE(field.Client->Joined());
	REQUIRE(field.Replica.Resource<engine::scene::WeatherClockInbox>());
	REQUIRE(field.Replica.Resource<engine::scene::WeatherClockInbox>()->Count == 1);
	// Continue snapshot delivery at the unchanged simulation tick after the
	// reliable pause message has already been consumed and acknowledged.
	for (int pump = 0; pump < 200 && !field.Client->Joined(); ++pump)
		field.Tick(1);
	REQUIRE(field.Client->Joined());
	const auto accepted = engine::scene::WeatherClockOf(field.Replica, provider);
	CHECK(accepted.Sequence == paused.Sequence);
	CHECK(accepted.Tick == paused.Tick);
	CHECK(accepted.ElapsedSeconds == paused.ElapsedSeconds);
	CHECK(accepted.ElapsedFractionQ32 == paused.ElapsedFractionQ32);
	CHECK(accepted.ElapsedRemainder == paused.ElapsedRemainder);
	CHECK(accepted.WorldPaused);
	CHECK_FALSE(accepted.Paused);
	CHECK(field.Replica.Resource<engine::scene::WeatherClockInbox>()->Count == 1);
	CHECK(field.Replica.Time().Elapsed == 0);
	CHECK(field.World.Time().Elapsed == 0);
}

TEST_CASE(
	"reliable weather heartbeats reach interested peers despite slow budgeted ordinary publication",
	"[replication][negotiation][weather]"
) {
	const auto mode = GENERATE(WireMode::Datagram, WireMode::Quic);
	engine::scene::RegisterSceneComponents();
	auto ends = MakeLoopbackTransport(4);
	REQUIRE(ends.size() == 4);
	ListenerSettings settings;
	settings.Wire = mode;
	settings.Quic.Connection.Tls.Seed = Seed();
	settings.Quic.Connection.Tls.HasSeed = true;
	// Joining snapshots have their own bounded chunks; ordinary component rows cannot fit.
	settings.Authority.BytesPerTick = 1;
	Listener server(*ends[0], settings);
	server.Authority().Replicate(Name("scene.WeatherClock"));
	std::vector<engine::replication::ClientId> admitted;
	server.OnAdmitted([&](auto client) { admitted.push_back(client); });
	engine::ecs::Store authority("weather_heartbeat_authority");
	const auto provider = authority.Create();
	engine::scene::WeatherClock clock;
	clock.Tick = 1000;
	clock.Sequence = 1;
	authority.Set(provider, clock);
	authority.AdvanceTick(0.05f);
	std::array<std::unique_ptr<engine::ecs::Store>, 3> replicas;
	std::array<std::unique_ptr<Connector>, 3> clients;
	std::array<uint32_t, 3> clockReceipts{};
	std::array<uint32_t, 3> otherReceipts{};
	double now = 0;
	for (size_t index = 0; index < clients.size(); ++index) {
		replicas[index] = std::make_unique<engine::ecs::Store>("weather_heartbeat_replica");
		clients[index] = std::make_unique<Connector>(*ends[index + 1], ends[0]->Local(), now);
		clients[index]->OnUserState([&, index](auto &store, auto bytes, double seconds, double rtt) {
			if (!engine::scene::AcceptWeatherClockUpdate(store, bytes, seconds, rtt)) return false;
			++clockReceipts[index];
			return true;
		});
		clients[index]->OnUserMessage([&, index](auto) { ++otherReceipts[index]; });
		clients[index]->OnTiming(engine::scene::SynchronizeWeatherClocks);
	}
	auto pump = [&](bool publish) {
		now += 1.0 / 60;
		for (size_t index = 0; index < clients.size(); ++index)
			clients[index]->Poll(*replicas[index], now);
		server.Poll(now);
		if (publish) server.Publish(authority, authority.Time().Tick, now);
		server.Advance(now);
		for (size_t index = 0; index < clients.size(); ++index) {
			clients[index]->Poll(*replicas[index], now);
			clients[index]->Advance(now);
		}
	};
	auto allJoined = [&] {
		return std::all_of(clients.begin(), clients.end(), [](const auto &client) {
			return client->Joined();
		});
	};
	for (uint32_t attempt = 0; attempt < 600 && !allJoined(); ++attempt)
		pump(true);
	REQUIRE(allJoined());
	REQUIRE(admitted.size() == 3);
	const auto excluded = admitted.back();
	server.Authority().SetInterest([excluded](auto client, auto, const auto &) {
		return client != excluded;
	});
	const auto simulationTick = authority.Time().Tick;
	for (uint32_t heartbeat = 1; heartbeat <= 4; ++heartbeat) {
		++clock.Sequence;
		clock.Tick += 20;
		authority.Set(provider, clock);
		engine::core::ByteWriter message;
		engine::scene::WriteWeatherClockUpdate(message, provider, clock);
		if (heartbeat == 1) {
			// Fill a real reliable lane, then retry the latest clock without advancing a world tick.
			// Equal user-payload lengths spend the same framed budget as the anchor itself.
			const std::vector<std::byte> filler(message.Size());
			uint32_t sent = 0;
			while (sent < 1024 && server.SendTo(admitted.front(), filler, now))
				++sent;
			REQUIRE(sent > 0);
			REQUIRE(sent < 1024);
			REQUIRE_FALSE(server.BroadcastEntityState(authority, provider, message.Bytes(), now, heartbeat));
			++clock.Sequence;
			clock.Tick += 1;
			authority.Set(provider, clock);
			message = engine::core::ByteWriter{};
			engine::scene::WriteWeatherClockUpdate(message, provider, clock);
		}
		bool accepted = false;
		for (uint32_t frame = 0; frame < 60; ++frame) {
			accepted |= server.BroadcastEntityState(authority, provider, message.Bytes(), now, heartbeat);
			// Ordinary publication is only once every three seconds, with an insufficient row budget.
			pump(heartbeat == 3 && frame == 0);
		}
		REQUIRE(accepted);
		uint32_t current = 0;
		uint32_t omitted = 0;
		for (size_t index = 0; index < replicas.size(); ++index) {
			const auto &replica = replicas[index];
			const auto *received = replica->Get<engine::scene::WeatherClock>(provider);
			if (clockReceipts[index] != 0) {
				CHECK(clockReceipts[index] == heartbeat);
				REQUIRE(received);
				if (received->Sequence == clock.Sequence) ++current;
				if (heartbeat == 1) {
					CHECK(received->Sequence >= clock.Sequence - 1);
					CHECK(received->Tick >= clock.Tick - 1);
				} else {
					CHECK(received->Sequence == clock.Sequence);
					CHECK(received->Tick == clock.Tick);
				}
			} else {
				++omitted;
				if (received) CHECK(received->Sequence == 1);
			}
		}
		CHECK(current == (heartbeat == 1 ? 1 : 2));
		CHECK(omitted == 1);
		CHECK(authority.Time().Tick == simulationTick);
	}
	// Coincident token and provider ids in another world must not suppress its copied state.
	engine::ecs::Store otherWorld("weather_heartbeat_other_world");
	REQUIRE(otherWorld.CreateAt(provider));
	const std::array<std::byte, 1> otherPayload{std::byte{7}};
	const auto beforeOther = otherReceipts;
	REQUIRE(server.BroadcastEntityState(otherWorld, provider, otherPayload, now, 4));
	REQUIRE(server.BroadcastEntityState(authority, provider, otherPayload, now, 4));
	for (uint32_t frame = 0; frame < 30; ++frame)
		pump(false);
	uint32_t otherDelivered = 0;
	for (size_t index = 0; index < clients.size(); ++index) {
		if (clockReceipts[index] == 0)
			CHECK(otherReceipts[index] == beforeOther[index]);
		else {
			CHECK(otherReceipts[index] == beforeOther[index] + 1);
			++otherDelivered;
		}
	}
	CHECK(otherDelivered == 2);
}

TEST_CASE(
	"tracked entity publications bound receipts and release retired or superseded slots",
	"[replication][negotiation]"
) {
	const auto mode = GENERATE(WireMode::Datagram, WireMode::Quic);
	Field field(mode);
	field.World.Set(field.World.Create(), Spot{});
	field.Connect({});
	for (uint32_t tick = 1; tick < 600 && !field.Client->Joined(); ++tick)
		field.Tick(tick);
	REQUIRE(field.Client->Joined());
	uint32_t receipts = 0;
	field.Client->OnUserMessage([&](auto) { ++receipts; });
	const std::array<std::byte, 1> payload{std::byte{1}};
	std::array<engine::ecs::Entity, 32> providers;
	uint64_t tick = 1000;
	for (auto &provider : providers) {
		provider = field.World.Create();
		bool accepted = false;
		for (uint32_t attempt = 0; attempt < 120 && !accepted; ++attempt) {
			accepted = field.Server->BroadcastEntityState(field.World, provider, payload, field.Now, 1);
			field.Tick(tick++);
		}
		REQUIRE(accepted);
	}
	for (uint32_t pump = 0; pump < 30; ++pump)
		field.Tick(tick++);
	CHECK(receipts == 32);
	const auto extra = field.World.Create();
	CHECK_FALSE(field.Server->BroadcastEntityState(field.World, extra, payload, field.Now, 1));
	field.World.Destroy(providers.front());
	REQUIRE(field.Server->BroadcastEntityState(field.World, extra, payload, field.Now, 1));
	field.Tick(tick++);
	const auto replacement = field.World.Create();
	REQUIRE(field.Server->BroadcastEntityState(field.World, replacement, payload, field.Now, 2));
	for (uint32_t retry = 0; retry < 30; ++retry) {
		REQUIRE(field.Server->BroadcastEntityState(field.World, replacement, payload, field.Now, 2));
		field.Tick(tick++);
	}
	CHECK(receipts == 34);
}
