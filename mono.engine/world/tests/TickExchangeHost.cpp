#include <engine/core/Bytes.hpp>
#include <engine/core/Paths.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/parallel/Process.hpp>
#include <engine/parallel/ProcessChannel.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Driver.hpp>
#include <engine/world/Supervisor.hpp>
#include <engine/world/TickExchangeHost.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <thread>

TEST_SUITE_ID("engine.world.tickexchangehost")
TEST_DEPENDS("engine.world.tickexchange")
TEST_DEPENDS("engine.world.hostlink")

using namespace engine;
namespace {
	struct Probe {
		int Inputs = 0;
		int Integrations = 0;
		int Contacts = 0;
		std::string Destination;
	};
	void Collect(ecs::Store &store, std::vector<world::TickExchangeRequest> &requests) {
		const auto &probe = *store.Resource<Probe>();
		if (probe.Destination.empty()) return;
		world::TickExchangeRequest request;
		request.Stamp.DestinationWorld = probe.Destination;
		request.Payload = {std::byte{17}};
		requests.push_back(std::move(request));
	}
	world::TickExchangeStatus
	Serve(ecs::Store &, const world::TickExchangeRequest &request, std::vector<std::byte> &reply) {
		reply = request.Payload;
		return world::TickExchangeStatus::Complete;
	}
	void Apply(ecs::Store &store, std::span<const world::TickExchangeReply> replies) {
		for (const auto &reply : replies)
			if (reply.Status == world::TickExchangeStatus::Complete &&
				reply.Payload == std::vector{std::byte{17}})
				store.ResourceMutable<Probe>()->Contacts++;
	}
	struct Host {
		Probe Observed;
		world::Universe Worlds;
		world::TickExchangeHost Exchange{Worlds};
		world::WorldId World;
		Host(const char *name, const char *destination, uint64_t incarnation, double tickRate = 60) {
			ecs::Components::Register<Probe>("test.HostExchangeProbe");
			REQUIRE(world::RegisterTickExchangeChannel({"test.host.contacts", Collect, Serve, Apply}));
			World = Worlds.Create({.Name = core::Name(name), .TickRate = tickRate});
			REQUIRE(World.IsValid());
			Worlds.Enter(World, [&](ecs::Store &store, ecs::Scheduler &systems) {
				Probe probe;
				probe.Destination = destination;
				store.SetResource(probe);
				REQUIRE(world::OpenTickExchange(store, "test.host.contacts", incarnation));
				systems.Add("input", ecs::Phase::Input, [this](ecs::Store &state) {
					state.ResourceMutable<Probe>()->Inputs++;
					Observed = *state.Resource<Probe>();
				});
				systems.Add("physics", ecs::Phase::Simulation, [this](ecs::Store &state) {
					state.ResourceMutable<Probe>()->Integrations++;
					Observed = *state.Resource<Probe>();
				});
			});
		}
		Probe Read() {
			if (Worlds.TickExchangeFrameOpen()) return Observed;
			Probe result;
			Worlds.Enter(World, [&](ecs::Store &store) { result = *store.Resource<Probe>(); });
			return result;
		}
		world::TickExchangeResult Send(const world::TickExchangeCommand &command) {
			core::ByteWriter request;
			REQUIRE(world::WriteTickExchangeControl(request, command));
			core::ByteReader bytes(request.Bytes());
			world::TickExchangeCommand decoded;
			REQUIRE(world::ReadTickExchangeControl(bytes, decoded));
			const auto result = Exchange.Handle(decoded);
			core::ByteWriter response;
			REQUIRE(world::WriteTickExchangeControl(response, result));
			core::ByteReader reply(response.Bytes());
			world::TickExchangeResult read;
			REQUIRE(world::ReadTickExchangeControl(reply, read));
			return read;
		}
	};
}

namespace {
	std::vector<std::byte> Receive(parallel::Channel &channel) {
		std::vector<std::byte> bytes;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (std::chrono::steady_clock::now() < deadline) {
			const auto status = channel.Receive(bytes);
			if (status == parallel::ChannelStatus::Ok) return bytes;
			REQUIRE(status == parallel::ChannelStatus::Empty);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		FAIL("host exchange response deadline expired");
		return {};
	}
}

static void RunHostChild(bool refuseServe, bool reciprocal = false) {
	auto channel = parallel::AdoptInheritedChannel();
	REQUIRE(channel);
	Host host("destination", reciprocal ? "source" : "", 202);
	world::HostFrame ready;
	ready.Signal = world::HostSignal::Ready;
	core::ByteWriter readyBytes;
	world::WriteHostFrame(readyBytes, ready);
	REQUIRE(channel->Send(readyBytes.Bytes()) == parallel::ChannelStatus::Ok);
	for (;;) {
		const auto bytes = Receive(*channel);
		core::ByteReader reader(bytes);
		world::HostFrame command;
		REQUIRE(world::ReadHostFrame(reader, command));
		REQUIRE(command.Signal == world::HostSignal::TickExchangeCommand);
		world::HostFrame result;
		result.Signal = world::HostSignal::TickExchangeResult;
		if (refuseServe && command.ExchangeCommand.Operation == world::TickExchangeOperation::Serve) {
			result.ExchangeResult.Operation = command.ExchangeCommand.Operation;
			result.ExchangeResult.Frame = command.ExchangeCommand.Frame;
			result.ExchangeResult.Round = command.ExchangeCommand.Round;
		} else
			result.ExchangeResult = host.Exchange.Handle(command.ExchangeCommand);
		core::ByteWriter response;
		world::WriteHostFrame(response, result);
		REQUIRE_FALSE(response.Empty());
		REQUIRE(channel->Send(response.Bytes()) == parallel::ChannelStatus::Ok);
		if ((command.ExchangeCommand.Operation == world::TickExchangeOperation::End ||
			 command.ExchangeCommand.Operation == world::TickExchangeOperation::Cancel) &&
			result.ExchangeResult.Success)
			break;
	}
	CHECK(host.Read().Inputs == 1);
	CHECK(host.Read().Integrations == (refuseServe ? 0 : 1));
	CHECK(host.Read().Contacts == (reciprocal && !refuseServe ? 1 : 0));
}
TEST_CASE("tick exchange host process child", "[.host-tick-exchange-child]") {
	RunHostChild(false);
}
TEST_CASE("tick exchange refusing host process child", "[.host-tick-exchange-child]") {
	RunHostChild(true);
}
TEST_CASE("tick exchange reciprocal host process child", "[.host-tick-exchange-child]") {
	RunHostChild(false, true);
}
TEST_CASE("tick exchange reciprocal refusing host process child", "[.host-tick-exchange-child]") {
	RunHostChild(true, true);
}

TEST_CASE(
	"host exchange receives contact bytes before integration across a process boundary",
	"[world][host-exchange][process]"
) {
	auto channels = parallel::MakeProcessChannel();
	REQUIRE(channels.Valid());
	parallel::Process child;
	REQUIRE(child.Start(
		core::Paths::Base() / core::Paths::Program("test_world"),
		{"tick exchange host process child"},
		std::move(channels.Remote)
	));
	world::Supervisor supervisor;
	supervisor.SetLauncher([](const world::HostPlan &, parallel::Process &) { return true; });
	const core::Name remoteHost("host.destination");
	REQUIRE(supervisor.Start({{remoteHost, {core::Name("destination")}}}) == 1);
	REQUIRE(supervisor.Attach(remoteHost, std::move(channels.Local)));
	const auto remote = [&](const world::TickExchangeCommand &command) {
		REQUIRE(supervisor.SendTickExchange(remoteHost, command));
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (std::chrono::steady_clock::now() < deadline) {
			supervisor.Pump(1.0);
			if (auto result = supervisor.TakeTickExchange(remoteHost)) return *result;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		FAIL("supervised host exchange response deadline expired");
		return world::TickExchangeResult{};
	};
	Host source("source", "destination", 101);
	world::TickExchangeCommand command;
	command.Frame = 19;
	command.FrameSeconds = 1.0f / 60;
	REQUIRE(source.Send(command).Rounds == 1);
	REQUIRE(remote(command).Rounds == 1);
	command.FrameSeconds = 0;
	command.Operation = world::TickExchangeOperation::Collect;
	const auto requests = source.Send(command);
	REQUIRE(requests.Success);
	REQUIRE(remote(command).Success);
	CHECK(source.Read().Integrations == 0);
	command.Operation = world::TickExchangeOperation::Serve;
	REQUIRE(source.Send(command).Success);
	command.Requests = requests.Requests;
	const auto replies = remote(command);
	REQUIRE(replies.Success);
	REQUIRE(replies.Replies.size() == 1);
	CHECK(replies.Replies.front().DestinationIncarnation == 202);
	CHECK(source.Read().Integrations == 0);
	command.Requests.clear();
	command.Operation = world::TickExchangeOperation::Apply;
	REQUIRE(remote(command).Success);
	command.Replies = replies.Replies;
	REQUIRE(source.Send(command).Success);
	CHECK(source.Read().Contacts == 1);
	CHECK(source.Read().Integrations == 1);
	command.Replies.clear();
	command.Operation = world::TickExchangeOperation::End;
	command.Round = 1;
	REQUIRE(source.Send(command).Success);
	REQUIRE(remote(command).Success);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (child.Poll().Alive() && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	const auto status = child.Poll();
	REQUIRE_FALSE(status.Alive());
	CHECK(status.Code == 0);
}

TEST_CASE("host phase handshake holds physics until copied replies arrive", "[world][host-exchange]") {
	Host source("source", "destination", 101), destination("destination", "", 202);
	world::TickExchangeCommand command;
	command.Frame = 1;
	command.FrameSeconds = 1.0f / 60;
	REQUIRE(source.Send(command).Rounds == 1);
	REQUIRE(destination.Send(command).Rounds == 1);
	REQUIRE(source.Send(command).Rounds == 1);
	command.FrameSeconds = 0;
	command.Operation = world::TickExchangeOperation::Collect;
	const auto requests = source.Send(command);
	REQUIRE(requests.Success);
	REQUIRE(requests.Requests.size() == 1);
	REQUIRE(source.Send(command).Requests.size() == 1);
	REQUIRE(destination.Send(command).Success);
	CHECK(source.Read().Inputs == 1);
	CHECK(source.Read().Integrations == 0);
	command.Operation = world::TickExchangeOperation::Apply;
	CHECK_FALSE(source.Send(command).Success);
	CHECK(source.Read().Integrations == 0);
	command.Operation = world::TickExchangeOperation::Serve;
	REQUIRE(source.Send(command).Success);
	command.Requests = requests.Requests;
	const auto replies = destination.Send(command);
	REQUIRE(replies.Success);
	REQUIRE(replies.Replies.size() == 1);
	CHECK(replies.Replies.front().DestinationIncarnation == 202);
	command.Requests.clear();
	command.Operation = world::TickExchangeOperation::Apply;
	REQUIRE(destination.Send(command).Success);
	command.Replies = replies.Replies;
	command.Replies.front().Stamp.SourceTick++;
	CHECK_FALSE(source.Send(command).Success);
	CHECK(source.Read().Integrations == 0);
	command.Replies = replies.Replies;
	REQUIRE(source.Send(command).Success);
	REQUIRE(source.Send(command).Success);
	CHECK(source.Read().Integrations == 1);
	CHECK(source.Read().Contacts == 1);
	command.Replies.clear();
	command.Operation = world::TickExchangeOperation::End;
	command.Round = 1;
	REQUIRE(source.Send(command).Success);
	REQUIRE(destination.Send(command).Success);
	REQUIRE_FALSE(source.Worlds.TickExchangeFrameOpen());
	command.Operation = world::TickExchangeOperation::Begin;
	command.Round = 0;
	command.FrameSeconds = 1.0f / 60;
	CHECK_FALSE(source.Send(command).Success);
	command.Frame++;
	REQUIRE(source.Send(command).Success);
	command.FrameSeconds = 0;
	command.Operation = world::TickExchangeOperation::Collect;
	REQUIRE(source.Send(command).Success);
	source.Exchange.Disconnect();
	CHECK_FALSE(source.Worlds.TickExchangeFrameOpen());
	CHECK(source.Read().Integrations == 1);
}

TEST_CASE(
	"host phase control rejects truncation direction confusion and oversized batches",
	"[world][host-exchange]"
) {
	world::TickExchangeCommand command;
	command.Operation = world::TickExchangeOperation::Serve;
	command.Frame = 7;
	command.Requests.push_back({{"a", "b", "contacts", 1, 2, 3}, {std::byte{9}}});
	core::ByteWriter encoded;
	REQUIRE(world::WriteTickExchangeControl(encoded, command));
	for (size_t count = 0; count < encoded.Size(); ++count) {
		core::ByteReader reader(encoded.Bytes().first(count));
		world::TickExchangeCommand sentinel;
		sentinel.Frame = 91;
		CHECK_FALSE(world::ReadTickExchangeControl(reader, sentinel));
		CHECK(sentinel.Frame == 91);
	}
	core::ByteReader wrongDirection(encoded.Bytes());
	world::TickExchangeResult result;
	CHECK_FALSE(world::ReadTickExchangeControl(wrongDirection, result));
	const auto before = encoded.Size();
	command.Requests.resize(world::MAXIMUM_TICK_EXCHANGE_MESSAGES + 1, command.Requests.front());
	CHECK_FALSE(world::WriteTickExchangeControl(encoded, command));
	CHECK(encoded.Size() == before);
	result.Operation = world::TickExchangeOperation::Serve;
	result.Frame = 7;
	result.Success = true;
	result.Replies.push_back(
		{{"a", "b", "contacts", 1, 2, 3}, 4, 5, world::TickExchangeStatus::Complete, {std::byte{9}}}
	);
	core::ByteWriter response;
	REQUIRE(world::WriteTickExchangeControl(response, result));
	for (size_t count = 0; count < response.Size(); ++count) {
		core::ByteReader reader(response.Bytes().first(count));
		world::TickExchangeResult sentinel;
		sentinel.Frame = 91;
		CHECK_FALSE(world::ReadTickExchangeControl(reader, sentinel));
		CHECK(sentinel.Frame == 91);
	}
}

TEST_CASE("a slower host serves a catch-up round without advancing its world", "[world][host-exchange]") {
	Host source("source", "destination", 101, 60), destination("destination", "", 202, 30);
	world::TickExchangeCommand command;
	command.Frame = 1;
	command.FrameSeconds = 1.0f / 30;
	REQUIRE(source.Send(command).Rounds == 2);
	REQUIRE(destination.Send(command).Rounds == 1);
	command.FrameSeconds = 0;
	for (uint32_t round = 0; round < 2; ++round) {
		command.Round = round;
		command.Operation = world::TickExchangeOperation::Collect;
		const auto requests = source.Send(command);
		REQUIRE(requests.Success);
		REQUIRE(destination.Send(command).Success);
		command.Operation = world::TickExchangeOperation::Serve;
		REQUIRE(source.Send(command).Success);
		command.Requests = requests.Requests;
		const auto replies = destination.Send(command);
		REQUIRE(replies.Success);
		REQUIRE(replies.Replies.size() == 1);
		CHECK(replies.Replies.front().DestinationTick == 1);
		command.Requests.clear();
		command.Operation = world::TickExchangeOperation::Apply;
		REQUIRE(destination.Send(command).Success);
		command.Replies = replies.Replies;
		REQUIRE(source.Send(command).Success);
		command.Replies.clear();
	}
	command.Operation = world::TickExchangeOperation::End;
	command.Round = 2;
	REQUIRE(source.Send(command).Success);
	REQUIRE(destination.Send(command).Success);
	CHECK(source.Read().Integrations == 2);
	CHECK(source.Read().Contacts == 2);
	CHECK(destination.Read().Integrations == 1);
	command.Operation = world::TickExchangeOperation::Begin;
	command.Frame = 2;
	command.Round = 0;
	command.FrameSeconds = 1.0f / 60;
	REQUIRE(source.Send(command).Success);
	command.FrameSeconds = 0;
	command.Operation = world::TickExchangeOperation::Collect;
	REQUIRE(source.Send(command).Success);
	command.Operation = world::TickExchangeOperation::Cancel;
	command.Round = 7;
	REQUIRE(source.Send(command).Success);
	REQUIRE(source.Send(command).Success);
	CHECK_FALSE(source.Worlds.TickExchangeFrameOpen());
	CHECK(source.Read().Integrations == 2);
}

TEST_CASE(
	"driver coordinates local contact collection with a supervised process", "[world][host-exchange][process]"
) {
	const bool refuseServe = GENERATE(false, true);
	const bool catchUp = GENERATE(false, true);
	world::DriverSettings settings;
	settings.CoordinateHostTicks = true;
	settings.Hosts.RestartLimit = 0;
	world::Driver driver(settings);
	auto channels = parallel::MakeProcessChannel();
	REQUIRE(channels.Valid());
	driver.Hosts().SetLauncher([&](const world::HostPlan &, parallel::Process &child) {
		return child.Start(
			core::Paths::Base() / core::Paths::Program("test_world"),
			{refuseServe ? "tick exchange reciprocal refusing host process child"
						 : "tick exchange reciprocal host process child"},
			std::move(channels.Remote)
		);
	});
	REQUIRE(driver.Start({{.Name = core::Name("destination"), .TickRate = 60}}) == 1);
	const auto remote = driver.Hosts().Hosts().front().Name;
	REQUIRE(driver.Hosts().Attach(remote, std::move(channels.Local)));
	const auto readyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!driver.Hosts().StatusOf(remote).Ready && std::chrono::steady_clock::now() < readyDeadline) {
		driver.Hosts().Pump(0);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	REQUIRE(driver.Hosts().StatusOf(remote).Ready);
	ecs::Components::Register<Probe>("test.HostExchangeProbe");
	REQUIRE(world::RegisterTickExchangeChannel({"test.host.contacts", Collect, Serve, Apply}));
	const auto local =
		driver.Worlds().Create({.Name = core::Name("source"), .TickRate = catchUp ? 120.0 : 60.0});
	driver.Worlds().Enter(local, [](ecs::Store &store, ecs::Scheduler &systems) {
		Probe probe;
		probe.Destination = "destination";
		store.SetResource(probe);
		REQUIRE(world::OpenTickExchange(store, "test.host.contacts", 101));
		systems.Add("input", ecs::Phase::Input, [](ecs::Store &state) {
			state.ResourceMutable<Probe>()->Inputs++;
		});
		systems.Add("physics", ecs::Phase::Simulation, [](ecs::Store &state) {
			auto *probe = state.ResourceMutable<Probe>();
			CHECK(probe->Contacts == probe->Inputs);
			probe->Integrations++;
		});
	});
	driver.Tick(1.0f / 60, 1);
	CHECK(driver.Statistics().TickExchangeFailed == refuseServe);
	CHECK_FALSE(driver.Worlds().TickExchangeFrameOpen());
	driver.Worlds().Enter(local, [refuseServe, catchUp](ecs::Store &store) {
		const auto *probe = store.Resource<Probe>();
		const int expected = catchUp ? 2 : 1;
		CHECK(probe->Inputs == (refuseServe ? 1 : expected));
		CHECK(probe->Integrations == (refuseServe ? 0 : expected));
		CHECK(probe->Contacts == (refuseServe ? 0 : expected));
	});
}
