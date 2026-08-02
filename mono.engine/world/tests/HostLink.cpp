// The protocol between a driver and a supervised host.
//
// Driven over a local channel rather than a real process. What a second process
// adds — a socket, a partial write, an exec — is `parallel`'s to test and it
// does; what is left here is the protocol itself, which is the part that has to
// be right whichever transport is underneath.

#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>
#include <engine/core/Random.hpp>
#include <engine/parallel/Channel.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/HostLink.hpp>
#include <engine/world/Supervisor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.world.hostlink")

using engine::core::ByteReader;
using engine::core::ByteWriter;
using engine::core::Name;
using engine::core::Random;
using engine::parallel::MakeLocalChannel;
using engine::world::Envelope;
using engine::world::HostDelivery;
using engine::world::HostFrame;
using engine::world::HostLink;
using engine::world::HostPlan;
using engine::world::HostSignal;
using engine::world::HostState;
using engine::world::Supervisor;
using engine::world::SupervisorSettings;

namespace hostlink_test {
	std::vector<std::byte> Bytes(std::string_view text) {
		std::vector<std::byte> payload(text.size());
		if (!text.empty()) {
			std::memcpy(payload.data(), text.data(), text.size());
		}
		return payload;
	}

	std::string Text(std::span<const std::byte> payload) {
		return std::string(reinterpret_cast<const char *>(payload.data()), payload.size());
	}

	Envelope Message(const char *key, const char *payload, uint64_t sequence) {
		Envelope envelope;
		envelope.Bus = engine::world::BusKind::Messaging;
		envelope.Operation = engine::world::BusOperation::Publish;
		envelope.Key = Name(key);
		envelope.From = Name("somewhere");
		envelope.Sequence = sequence;
		envelope.Payload = Bytes(payload);
		return envelope;
	}

	// A connected pair of links, standing in for a driver and one host.
	struct Pair {
		Pair() {
			auto [first, second] = MakeLocalChannel();
			Driver = std::make_unique<HostLink>(std::move(first), Name("driver"));
			Host = std::make_unique<HostLink>(std::move(second), Name("host.one"));
		}

		std::unique_ptr<HostLink> Driver;
		std::unique_ptr<HostLink> Host;
	};
}

using namespace hostlink_test;

TEST_CASE("a frame survives a round trip through the encoding", "[world]") {
	HostFrame written;
	written.Signal = HostSignal::Traffic;
	written.Host = Name("host.seven");
	written.World = Name("lobby");
	written.Tick = 987654321;
	written.Traffic.push_back(Message("topic.a", "first", 1));
	written.Traffic.push_back(Message("topic.b", "second", 2));
	written.Deliveries.push_back(HostDelivery{Name("lobby"), {}});

	ByteWriter writer;
	WriteHostFrame(writer, written);

	ByteReader reader(writer.Bytes());
	HostFrame read;
	REQUIRE(ReadHostFrame(reader, read));

	REQUIRE(read.Signal == written.Signal);
	REQUIRE(read.Host == written.Host);
	REQUIRE(read.World == written.World);
	REQUIRE(read.Tick == written.Tick);
	REQUIRE(read.Traffic.size() == 2);
	REQUIRE(read.Traffic[0].Key == Name("topic.a"));
	REQUIRE(Text(read.Traffic[1].Payload) == "second");
	REQUIRE(read.Deliveries.size() == 1);
	REQUIRE(read.Deliveries[0].World == Name("lobby"));
}

TEST_CASE("a frame that is not one is refused rather than half-read", "[world]") {
	std::vector<std::byte> rubbish(96);
	for (size_t index = 0; index < rubbish.size(); index++) {
		rubbish[index] = static_cast<std::byte>(Random::Bits(7u, static_cast<uint32_t>(index)));
	}

	ByteReader reader(rubbish);
	HostFrame frame;
	frame.Tick = 42;

	REQUIRE_FALSE(ReadHostFrame(reader, frame));

	// Untouched, so a caller reusing one frame across a poll loop cannot end up
	// acting on a mixture of the last good frame and a bad one.
	REQUIRE(frame.Tick == 42);
}

TEST_CASE("a truncated frame is refused", "[world]") {
	HostFrame written;
	written.Signal = HostSignal::Traffic;
	written.Traffic.push_back(Message("topic", "payload", 1));

	ByteWriter writer;
	WriteHostFrame(writer, written);

	const std::span<const std::byte> whole = writer.Bytes();
	for (size_t length = 1; length < whole.size(); length++) {
		ByteReader reader(whole.subspan(0, length));
		HostFrame read;
		REQUIRE_FALSE(ReadHostFrame(reader, read));
	}
}

TEST_CASE("a link stamps the sender rather than trusting it", "[world]") {
	// The same rule the driver applies to `Envelope::From`. An endpoint does
	// not get to say who it is when everything downstream keys on the answer.
	Pair pair;

	HostFrame frame;
	frame.Signal = HostSignal::Heartbeat;
	frame.Host = Name("somebody.else"); // ignored: already set, so kept
	REQUIRE(pair.Host->Send(frame));

	HostFrame unnamed;
	unnamed.Signal = HostSignal::Heartbeat;
	REQUIRE(pair.Host->Send(unnamed));

	std::vector<HostFrame> got;
	REQUIRE(pair.Driver->Receive(got) == 2);
	REQUIRE(got[0].Host == Name("somebody.else"));
	REQUIRE(got[1].Host == Name("host.one"));
}

TEST_CASE("a quiet tick sends nothing at all", "[world]") {
	// A universe is quiet most ticks. A frame per quiet tick is a frame per
	// tick, and the other end has to read every one of them.
	Pair pair;

	REQUIRE(pair.Host->SendTraffic({}));
	REQUIRE(pair.Host->SendDeliveries({}));

	std::vector<HostFrame> got;
	REQUIRE(pair.Driver->Receive(got) == 0);
	REQUIRE(got.empty());
}

TEST_CASE("traffic crosses in the order it was posted", "[world]") {
	Pair pair;

	std::vector<Envelope> traffic;
	for (int index = 0; index < 32; index++) {
		traffic.push_back(Message("churn", std::to_string(index).c_str(), static_cast<uint64_t>(index)));
	}
	REQUIRE(pair.Host->SendTraffic(traffic));

	std::vector<HostFrame> got;
	REQUIRE(pair.Driver->Receive(got) == 1);
	REQUIRE(got[0].Signal == HostSignal::Traffic);
	REQUIRE(got[0].Traffic.size() == 32);

	for (int index = 0; index < 32; index++) {
		REQUIRE(Text(got[0].Traffic[index].Payload) == std::to_string(index));
		REQUIRE(got[0].Traffic[index].Sequence == static_cast<uint64_t>(index));
	}
}

TEST_CASE("receive drains everything waiting rather than one frame", "[world]") {
	// The caller is a barrier that runs once a tick. A backlog left behind is a
	// backlog that grows.
	Pair pair;

	for (int index = 0; index < 25; index++) {
		REQUIRE(pair.Host->Heartbeat(static_cast<uint64_t>(index)));
	}

	std::vector<HostFrame> got;
	REQUIRE(pair.Host->Receive(got) == 0); // its own frames are not its own to read
	REQUIRE(pair.Driver->Receive(got) == 25);
	REQUIRE(got.size() == 25);
	REQUIRE(got.back().Tick == 24);

	// Appended rather than cleared, so a caller may accumulate.
	REQUIRE(pair.Driver->Receive(got) == 0);
	REQUIRE(got.size() == 25);
}

TEST_CASE("a link over a closed channel reports it rather than pretending", "[world]") {
	Pair pair;
	REQUIRE(pair.Driver->Connected());

	pair.Host->Close();
	REQUIRE_FALSE(pair.Driver->Connected());
	REQUIRE_FALSE(pair.Driver->Heartbeat(1));
	REQUIRE(pair.Driver->Dropped() == 1);
}

TEST_CASE("a link with no channel is a no-op rather than a crash", "[world]") {
	// What an unsupervised process holds. Every call has to be safe, because
	// the code above it does not branch on whether it is supervised.
	HostLink alone(nullptr, Name("nobody"));

	REQUIRE_FALSE(alone.Connected());
	REQUIRE_FALSE(alone.Heartbeat(1));

	std::vector<HostFrame> got;
	REQUIRE(alone.Receive(got) == 0);
	alone.Close();
}

TEST_CASE("garbage on the wire is discarded and counted, not acted on", "[world]") {
	auto [first, second] = MakeLocalChannel();
	HostLink driver(std::move(first), Name("driver"));

	// Something that is not a frame at all, straight onto the channel.
	std::vector<std::byte> rubbish(64);
	for (size_t index = 0; index < rubbish.size(); index++) {
		rubbish[index] = static_cast<std::byte>(Random::Bits(11u, static_cast<uint32_t>(index)));
	}
	second->Send(rubbish);

	// And one real frame behind it, which must still get through.
	HostLink host(std::move(second), Name("host.one"));
	REQUIRE(host.Heartbeat(99));

	std::vector<HostFrame> got;
	REQUIRE(driver.Receive(got) == 1);
	REQUIRE(got[0].Tick == 99);
	REQUIRE(driver.Malformed() == 1);
}

// --- the supervisor's side ---------------------------------------------------

namespace hostlink_test {
	// A supervisor with one host and a link to it, without a process.
	struct Supervised {
		Supervised() {
			SupervisorSettings settings;
			settings.HeartbeatSeconds = 5.0;
			Driver = std::make_unique<Supervisor>(settings);

			// Nothing spawned: the launcher succeeds and the link is attached
			// by hand, which is what `Attach` is for.
			Driver->SetLauncher([](const HostPlan &, engine::parallel::Process &) { return true; });

			HostPlan plan;
			plan.Name = Name("host.one");
			plan.Worlds = {Name("lobby"), Name("arena")};
			REQUIRE(Driver->Start({plan}) == 1);

			auto [driverEnd, hostEnd] = MakeLocalChannel();
			REQUIRE(Driver->Attach(Name("host.one"), std::move(driverEnd)));
			Host = std::make_unique<HostLink>(std::move(hostEnd), Name("host.one"));
		}

		std::unique_ptr<Supervisor> Driver;
		std::unique_ptr<HostLink> Host;
	};
}

TEST_CASE("a heartbeat over the link is the same event as one reported by hand", "[world]") {
	// The claim the whole link rests on: where a world runs is a deployment
	// decision, so nothing downstream may distinguish the two.
	Supervised pair;

	REQUIRE(pair.Host->Heartbeat(7));
	REQUIRE(pair.Driver->Pump(1.0) == 1);

	const auto status = pair.Driver->StatusOf(Name("host.one"));
	REQUIRE(status.State == HostState::Running);
	REQUIRE(status.Tick == 7);
	REQUIRE(status.Linked);

	// And the deadline is satisfied by it, which is the point.
	REQUIRE(pair.Driver->Poll(3.0) == 0);
	REQUIRE(pair.Driver->StatusOf(Name("host.one")).State == HostState::Running);
}

TEST_CASE("a host that answers while its tick stands still is visible", "[world]") {
	// A heartbeat says the link is being serviced. The tick says the simulation
	// is. A host wedged inside a system answers the first and not the second,
	// and a deadline alone would never notice.
	Supervised pair;

	for (int step = 0; step < 10; step++) {
		REQUIRE(pair.Host->Heartbeat(4));
		pair.Driver->Pump(static_cast<double>(step));
		REQUIRE(pair.Driver->Poll(static_cast<double>(step)) == 0);
	}

	const auto status = pair.Driver->StatusOf(Name("host.one"));
	REQUIRE(status.State == HostState::Running);
	REQUIRE(status.Tick == 4);
}

TEST_CASE("a ready frame is recorded and is a heartbeat too", "[world]") {
	Supervised pair;
	REQUIRE_FALSE(pair.Driver->StatusOf(Name("host.one")).Ready);

	HostFrame ready;
	ready.Signal = HostSignal::Ready;
	ready.Tick = 0;
	REQUIRE(pair.Host->Send(ready));
	REQUIRE(pair.Driver->Pump(1.0) == 1);

	REQUIRE(pair.Driver->StatusOf(Name("host.one")).Ready);
}

TEST_CASE("traffic a host posted reaches the driver in arrival order", "[world]") {
	Supervised pair;

	std::vector<Envelope> first{Message("a", "1", 1), Message("a", "2", 2)};
	std::vector<Envelope> second{Message("b", "3", 3)};
	REQUIRE(pair.Host->SendTraffic(first));
	REQUIRE(pair.Host->SendTraffic(second));

	REQUIRE(pair.Driver->Pump(1.0) == 2);
	REQUIRE(pair.Driver->Traffic().size() == 3);

	const std::vector<engine::world::HostTraffic> taken = pair.Driver->TakeTraffic();
	REQUIRE(taken.size() == 3);
	REQUIRE(Text(taken[0].Message.Payload) == "1");
	REQUIRE(Text(taken[2].Message.Payload) == "3");

	// Tagged with the host that sent it, not with what the frame claimed. The
	// router's check is "this host holds that world" and it cannot be made
	// against a field the sender wrote.
	REQUIRE(taken[0].Host == Name("host.one"));

	// Taken means taken. A second barrier must not re-apply the same requests.
	REQUIRE(pair.Driver->Traffic().empty());
}

TEST_CASE("a driver can send deliveries and a stop", "[world]") {
	Supervised pair;

	std::vector<HostDelivery> deliveries;
	deliveries.push_back(HostDelivery{Name("lobby"), {}});
	deliveries.back().Message.Key = Name("answer");
	deliveries.back().Message.Payload = Bytes("value");

	REQUIRE(pair.Driver->DeliverTo(Name("host.one"), deliveries));
	REQUIRE(pair.Driver->AskToStop(Name("host.one")));

	std::vector<HostFrame> got;
	REQUIRE(pair.Host->Receive(got) == 2);
	REQUIRE(got[0].Signal == HostSignal::Deliveries);
	REQUIRE(got[0].Deliveries.size() == 1);
	REQUIRE(got[0].Deliveries[0].World == Name("lobby"));
	REQUIRE(Text(got[0].Deliveries[0].Message.Payload) == "value");
	REQUIRE(got[1].Signal == HostSignal::Stop);
}

TEST_CASE("a host whose link closes is treated as dead", "[world]") {
	// Sooner than the deadline, and it works for an in-process host that has no
	// child to reap. The deadline stays as the answer for one that is wedged
	// rather than gone.
	Supervised pair;

	REQUIRE(pair.Host->Heartbeat(1));
	REQUIRE(pair.Driver->Pump(1.0) == 1);
	REQUIRE(pair.Driver->StatusOf(Name("host.one")).State == HostState::Running);

	pair.Host->Close();

	// Well inside the five-second deadline, so nothing but the closed link can
	// be what noticed.
	REQUIRE(pair.Driver->Poll(1.5) == 1);
	REQUIRE(pair.Driver->StatusOf(Name("host.one")).Restarts == 1);
}

TEST_CASE("an unknown host cannot be sent to or attached", "[world]") {
	Supervised pair;

	REQUIRE_FALSE(pair.Driver->SendTo(Name("nobody"), {}));
	REQUIRE_FALSE(pair.Driver->AskToStop(Name("nobody")));
	REQUIRE_FALSE(pair.Driver->DeliverTo(Name("nobody"), {}));

	auto [first, second] = MakeLocalChannel();
	REQUIRE_FALSE(pair.Driver->Attach(Name("nobody"), std::move(first)));
}

TEST_CASE("a host reporting a world held down says which one", "[world]") {
	Supervised pair;

	HostFrame faulted;
	faulted.Signal = HostSignal::Faulted;
	faulted.World = Name("arena");
	REQUIRE(pair.Host->Send(faulted));

	// Twice, to prove it is not counted twice: a host repeating itself every
	// tick would otherwise grow the list without bound.
	REQUIRE(pair.Host->Send(faulted));
	REQUIRE(pair.Driver->Pump(1.0) == 2);

	REQUIRE(pair.Driver->HeldDown().size() == 1);
	REQUIRE(pair.Driver->HeldDown()[0] == Name("arena"));
}

TEST_CASE("a host sending what only a driver sends is refused", "[world]") {
	// Not merged, not acted on. A host answering its own worlds' bus requests
	// would be the second source of truth the design exists to avoid.
	Supervised pair;

	std::vector<HostDelivery> deliveries;
	deliveries.push_back(HostDelivery{Name("lobby"), {}});
	REQUIRE(pair.Host->SendDeliveries(deliveries));

	HostFrame stop;
	stop.Signal = HostSignal::Stop;
	REQUIRE(pair.Host->Send(stop));

	REQUIRE(pair.Driver->Pump(1.0) == 2);
	REQUIRE(pair.Driver->Traffic().empty());
	REQUIRE(pair.Driver->HeldDown().empty());
}

TEST_CASE("a link carries whatever a fuzzer puts through it", "[world][fuzz]") {
	// Payloads and names of every shape, in both directions, with the reader
	// asserting on what the writer said rather than on a fixed expectation.
	Pair pair;

	for (uint32_t round = 0; round < 400; round++) {
		HostFrame sent;
		sent.Signal = static_cast<HostSignal>(Random::Bits(round, 1) % 5);
		sent.World = Name("world." + std::to_string(Random::Bits(round, 2) % 8));
		sent.Tick = Random::Bits(round, 3);

		const size_t count = Random::Bits(round, 4) % 6;
		for (size_t index = 0; index < count; index++) {
			const size_t length = Random::Bits(round, static_cast<uint32_t>(10 + index)) % 300;
			sent.Traffic.push_back(Message("k", std::string(length, 'z').c_str(), index));
		}

		REQUIRE(pair.Host->Send(sent));

		std::vector<HostFrame> got;
		REQUIRE(pair.Driver->Receive(got) == 1);
		REQUIRE(got[0].Signal == sent.Signal);
		REQUIRE(got[0].World == sent.World);
		REQUIRE(got[0].Tick == sent.Tick);
		REQUIRE(got[0].Traffic.size() == sent.Traffic.size());
		for (size_t index = 0; index < count; index++) {
			REQUIRE(got[0].Traffic[index].Payload == sent.Traffic[index].Payload);
		}
	}
}
