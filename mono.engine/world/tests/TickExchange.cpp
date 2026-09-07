#include <engine/core/Bytes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/TickExchange.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.world.tickexchange")
using namespace engine;
namespace {
	struct Fixture {
		int Applied = 0;
		std::vector<world::TickExchangeReply> Replies;
	};
	void Collect(ecs::Store &, std::vector<world::TickExchangeRequest> &out) {
		world::TickExchangeRequest request;
		request.Stamp = {"spoofed", "destination", "spoofed.channel", 999, 999, 999};
		request.Payload = {std::byte{3}, std::byte{7}};
		out.push_back(std::move(request));
	}
	world::TickExchangeStatus
	Serve(ecs::Store &, const world::TickExchangeRequest &request, std::vector<std::byte> &out) {
		out = request.Payload;
		out.push_back(std::byte{11});
		return world::TickExchangeStatus::Complete;
	}
	void Apply(ecs::Store &store, std::span<const world::TickExchangeReply> replies) {
		auto *fixture = store.ResourceMutable<Fixture>();
		if (!fixture) return;
		fixture->Applied++;
		fixture->Replies.assign(replies.begin(), replies.end());
	}
	void Register() {
		world::RegisterTickExchangeComponents();
		ecs::Components::Register<Fixture>("test.TickExchangeFixture");
		REQUIRE(world::RegisterTickExchangeChannel({"contacts.test", Collect, Serve, Apply}));
	}
}
TEST_CASE(
	"tick exchange copies bytes stamps source identity and rejects stale replies", "[world][tick-exchange]"
) {
	Register();
	ecs::Store source("source"), destination("destination");
	REQUIRE(world::OpenTickExchange(source, "contacts.test", 101));
	REQUIRE(world::OpenTickExchange(destination, "contacts.test", 202));
	source.SetResource(Fixture{});
	source.AdvanceTick(1.0f / 60);
	std::vector<world::TickExchangeRequest> requests;
	REQUIRE(world::CollectTickExchanges(source, "source", requests));
	REQUIRE(requests.size() == 1);
	const auto &stamp = requests[0].Stamp;
	REQUIRE(stamp.SourceWorld == "source");
	REQUIRE(stamp.Channel == "contacts.test");
	REQUIRE(stamp.SourceIncarnation == 101);
	REQUIRE(stamp.SourceTick == 1);
	REQUIRE(stamp.Sequence == 1);
	const auto reply = world::ServeTickExchange(destination, requests[0]);
	REQUIRE(reply.Status == world::TickExchangeStatus::Complete);
	REQUIRE(reply.DestinationIncarnation == 202);
	REQUIRE(reply.DestinationTick == 0);
	REQUIRE(reply.Payload.size() == 3);
	REQUIRE(world::ApplyTickExchanges(source, std::span(&reply, 1)));
	REQUIRE(source.Resource<Fixture>()->Applied == 1);
	auto stale = reply;
	stale.Stamp.SourceIncarnation++;
	REQUIRE_FALSE(world::ApplyTickExchanges(source, std::span(&stale, 1)));
	REQUIRE(source.Resource<Fixture>()->Applied == 1);
	source.AdvanceTick(1.0f / 60);
	REQUIRE_FALSE(world::ApplyTickExchanges(source, std::span(&reply, 1)));
}
TEST_CASE("tick exchange codec rejects every truncation transactionally", "[world][tick-exchange]") {
	const world::TickExchangeRequest request{
		{"source", "destination", "contacts.test", 101, 3, 7}, {std::byte{1}, std::byte{2}}
	};
	core::ByteWriter bytes;
	REQUIRE(world::WriteTickExchange(bytes, request));
	for (size_t size = 0; size < bytes.Size(); ++size) {
		core::ByteReader reader(bytes.Bytes().first(size));
		world::TickExchangeRequest out;
		out.Stamp.SourceWorld = "sentinel";
		REQUIRE_FALSE(world::ReadTickExchange(reader, out));
		REQUIRE(out.Stamp.SourceWorld == "sentinel");
	}
	core::ByteReader complete(bytes.Bytes());
	world::TickExchangeRequest decoded;
	REQUIRE(world::ReadTickExchange(complete, decoded));
	REQUIRE(complete.AtEnd());
	REQUIRE(decoded.Payload == request.Payload);
	world::TickExchangeReply reply;
	reply.Stamp = request.Stamp;
	reply.Status = world::TickExchangeStatus::Cancelled;
	core::ByteWriter response;
	REQUIRE(world::WriteTickExchange(response, reply));
	for (size_t size = 0; size < response.Size(); ++size) {
		core::ByteReader reader(response.Bytes().first(size));
		world::TickExchangeReply out;
		out.Stamp.Channel = "sentinel";
		REQUIRE_FALSE(world::ReadTickExchange(reader, out));
		REQUIRE(out.Stamp.Channel == "sentinel");
	}
	auto invalid = request;
	invalid.Payload.resize(world::MAXIMUM_TICK_EXCHANGE_BYTES + 1);
	const auto before = bytes.Size();
	REQUIRE_FALSE(world::WriteTickExchange(bytes, invalid));
	REQUIRE(bytes.Size() == before);
}

namespace {
	struct PhaseTrace {
		uint64_t Input = 0;
		uint64_t Observed = 0;
		uint64_t Integrated = 0;
		uint64_t Arrivals = 0;
	};
	void CollectPhase(ecs::Store &, std::vector<world::TickExchangeRequest> &out) {
		world::TickExchangeRequest request;
		request.Stamp.DestinationWorld = "phase-destination";
		out.push_back(std::move(request));
	}
	world::TickExchangeStatus
	ServePhase(ecs::Store &store, const world::TickExchangeRequest &, std::vector<std::byte> &out) {
		core::ByteWriter bytes;
		bytes.WriteUInt64(store.Resource<PhaseTrace>()->Input);
		out.assign(bytes.Bytes().begin(), bytes.Bytes().end());
		return world::TickExchangeStatus::Complete;
	}
	void ApplyPhase(ecs::Store &store, std::span<const world::TickExchangeReply> replies) {
		auto *trace = store.ResourceMutable<PhaseTrace>();
		for (const auto &reply : replies) {
			core::ByteReader bytes(reply.Payload);
			trace->Observed = bytes.ReadUInt64();
			trace->Arrivals++;
		}
	}
	void InstallPhase(ecs::Store &store, ecs::Scheduler &systems) {
		store.SetResource(PhaseTrace{});
		REQUIRE(world::OpenTickExchange(store, "phase.test", 303));
		systems.Add("phase.intent", ecs::Phase::Input, [](ecs::Store &world) {
			world.ResourceMutable<PhaseTrace>()->Input = world.Time().Tick;
		});
		systems.Add("phase.integrate", ecs::Phase::Simulation, [](ecs::Store &world) {
			auto *trace = world.ResourceMutable<PhaseTrace>();
			trace->Integrated += trace->Observed;
		});
	}
	void RegisterPhase() {
		ecs::Components::Register<PhaseTrace>("test.TickExchangePhaseTrace");
		REQUIRE(world::RegisterTickExchangeChannel({"phase.test", CollectPhase, ServePhase, ApplyPhase}));
	}
}
TEST_CASE(
	"phase exchange joins input before serving and catch-up round before next input", "[world][tick-exchange]"
) {
	RegisterPhase();
	for (const auto mode : {world::ExecutionMode::WorldSerial, world::ExecutionMode::WorldParallel}) {
		world::UniverseSettings configuration;
		configuration.Mode = mode;
		configuration.WorldParallelFloorMilliseconds = 0;
		world::Universe worlds(configuration);
		world::WorldSettings settings;
		settings.Name = core::Name("phase-source");
		const auto source = worlds.Create(settings);
		settings.Name = core::Name("phase-destination");
		const auto destination = worlds.Create(settings);
		worlds.Enter(source, InstallPhase);
		worlds.Enter(destination, InstallPhase);
		worlds.Tick(3.0f / 60);
		for (const auto id : {source, destination})
			worlds.Enter(id, [&](ecs::Store &store) {
				const auto *trace = store.Resource<PhaseTrace>();
				REQUIRE(store.Time().Tick == 3);
				REQUIRE(trace->Input == 3);
				REQUIRE(trace->Observed == 3);
				REQUIRE(trace->Integrated == 6);
				REQUIRE(trace->Arrivals == 3);
			});
	}
}
TEST_CASE(
	"phase exchange refuses missing duplicate changed replies and cancellation never integrates",
	"[world][tick-exchange]"
) {
	RegisterPhase();
	world::Universe worlds;
	world::WorldSettings settings;
	settings.Name = core::Name("phase-source");
	const auto source = worlds.Create(settings);
	settings.Name = core::Name("phase-destination");
	const auto destination = worlds.Create(settings);
	worlds.Enter(source, InstallPhase);
	worlds.Enter(destination, InstallPhase);
	REQUIRE(worlds.BeginTickExchangeFrame(1.0f / 60) == 1);
	REQUIRE(worlds.BeginTickExchangeRound());
	std::vector<world::TickExchangeRequest> requests;
	std::vector<world::TickExchangeReply> replies;
	REQUIRE(worlds.CollectTickExchangeRequests(requests));
	REQUIRE(worlds.ServeTickExchangeRequests(requests, replies));
	REQUIRE(replies.size() == 2);
	core::ByteWriter snapshot;
	snapshot.WriteUInt8(99);
	REQUIRE_FALSE(worlds.Save(snapshot));
	REQUIRE(snapshot.Size() == 1);
	REQUIRE_FALSE(worlds.FinishTickExchangeRound());
	REQUIRE_FALSE(worlds.ApplyTickExchangeReplies(std::span(replies).first(1)));
	auto duplicate = replies;
	duplicate[1] = duplicate[0];
	REQUIRE_FALSE(worlds.ApplyTickExchangeReplies(duplicate));
	auto changed = replies;
	changed[0].Stamp.DestinationWorld = "spoofed";
	REQUIRE_FALSE(worlds.ApplyTickExchangeReplies(changed));
	worlds.CancelTickExchangeFrame();
	REQUIRE_FALSE(worlds.TickExchangeFrameOpen());
	for (const auto id : {source, destination})
		worlds.Enter(id, [&](ecs::Store &store) {
			REQUIRE(store.Resource<PhaseTrace>()->Input == 1);
			REQUIRE(store.Resource<PhaseTrace>()->Integrated == 0);
			REQUIRE(store.Resource<PhaseTrace>()->Arrivals == 0);
		});
}
