#include <engine/core/Paths.hpp>
#include <engine/parallel/Process.hpp>
#include <engine/parallel/ProcessChannel.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Driver.hpp>
#include <engine/world/HostLink.hpp>
#include <engine/world/Postbox.hpp>
#include <engine/world/PresentationBus.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <thread>

TEST_SUITE_ID("engine.world.presentationbus")
TEST_DEPENDS("engine.world.hostlink")
TEST_DEPENDS("engine.world.universe")

namespace {
	using namespace engine;
	using namespace engine::world;
	using core::Name;
	using Status = PresentationStatus;

	WorldSettings Named(const char *name) {
		WorldSettings settings;
		settings.Name = Name(name);
		return settings;
	}

	PresentationAddress Open(PresentationBus &bus, const char *world) {
		const auto opened = bus.Open(Name(world), Name("image"));
		REQUIRE(opened.Status == Status::Ok);
		return opened.Address;
	}

	bool Await(HostLink &link, std::vector<HostFrame> &frames) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < deadline) {
			if (link.Receive(frames) > 0) {
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return false;
	}
}

TEST_CASE(
	"presentation owns payloads and rejects expired source and destination receipts", "[world][presentation]"
) {
	PresentationBus bus(1);
	const auto source = Open(bus, "source");
	const auto destination = Open(bus, "destination");
	std::vector<std::byte> bytes{std::byte{42}, std::byte{8}};
	REQUIRE(bus.Send(source, destination, 91, bytes) == Status::Ok);
	bytes[0] = std::byte{0};
	CHECK(bus.Queued().Bytes == 2);
	CHECK(bus.Traffic().EnqueuedMessages == 1);
	CHECK(bus.Traffic().EnqueuedBytes == 2);
	auto messages = bus.Take(destination);
	REQUIRE(messages.size() == 1);
	CHECK(messages[0].From == source);
	CHECK(messages[0].To == destination);
	CHECK(messages[0].Sequence == 1);
	CHECK(messages[0].Correlation == 91);
	CHECK(messages[0].Payload[0] == std::byte{42});
	CHECK(bus.Queued().Bytes == 0);
	REQUIRE(bus.Send(source, destination, 92, bytes) == Status::Ok);
	REQUIRE(bus.Close(destination) == Status::Ok);
	CHECK(bus.Queued().Messages == 0);
	const auto reopened = Open(bus, "destination");
	CHECK(reopened.Generation != destination.Generation);
	CHECK(bus.Send(source, destination, 93, bytes) == Status::StaleEndpoint);
	CHECK(bus.Close(destination) == Status::StaleEndpoint);
	REQUIRE(bus.Close(source) == Status::Ok);
	const auto newSource = Open(bus, "source");
	CHECK(bus.Send(source, reopened, 94, bytes) == Status::StaleEndpoint);
	REQUIRE(bus.Send(newSource, reopened, 95, bytes) == Status::Ok);
	bus.RemoveWorld(Name("source"));
	CHECK(bus.Take(reopened).empty());
	CHECK(bus.Queued().Messages == 0);
	CHECK(bus.Traffic().EnqueuedMessages == 3);
	CHECK(bus.Traffic().EnqueuedBytes == 6);
	CHECK(bus.Traffic().TakenMessages == 1);
	CHECK(bus.Traffic().TakenBytes == 2);
	CHECK(bus.Traffic().DiscardedMessages == 2);
	CHECK(bus.Traffic().DiscardedBytes == 4);
}

TEST_CASE(
	"presentation byte message endpoint and wire limits refuse before queue growth", "[world][presentation]"
) {
	PresentationLimits limits;
	limits.Endpoints = 3;
	limits.MessagesPerEndpoint = 2;
	limits.BytesPerEndpoint = 3;
	limits.Messages = 3;
	limits.Bytes = 4;
	limits.MaximumPayload = 3;
	PresentationBus bus(8, limits);
	const auto source = Open(bus, "source");
	const auto first = Open(bus, "first");
	const auto second = Open(bus, "second");
	CHECK(bus.Open(Name("fourth"), Name("image")).Status == Status::Full);
	CHECK(bus.Open(Name("source"), Name("image")).Status == Status::Duplicate);
	std::vector<std::byte> two(2);
	REQUIRE(bus.Send(source, first, 0, two) == Status::Ok);
	CHECK(bus.Send(source, first, 0, two) == Status::Full);
	REQUIRE(bus.Send(source, second, 0, two) == Status::Ok);
	CHECK(bus.Send(source, first, 0, std::span(two).first(1)) == Status::Full);
	CHECK(bus.Queued().Bytes == 4);
	CHECK(bus.Send(source, first, 0, std::vector<std::byte>(4)) == Status::TooLarge);
	(void)bus.Take(first);
	(void)bus.Take(second);
	REQUIRE(bus.Send(source, first, 0, {}) == Status::Ok);
	REQUIRE(bus.Send(source, first, 0, {}) == Status::Ok);
	CHECK(bus.Send(source, first, 0, {}) == Status::Full);
	REQUIRE(bus.Send(source, second, 0, {}) == Status::Ok);
	CHECK(bus.Send(source, second, 0, {}) == Status::Full);
	CHECK(bus.Queued().Messages == 3);
	CHECK_FALSE(bus.Reset(8));
	CHECK_FALSE(bus.Reset(7));
	REQUIRE(bus.Reset(9));
	CHECK(bus.Queued().Messages == 0);
	const auto fresh = Open(bus, "source");
	CHECK(fresh.Session == 9);
	CHECK(bus.Send(source, fresh, 0, {}) == Status::StaleEndpoint);
	PresentationBus disabled;
	CHECK(disabled.Open(Name("source"), Name("image")).Status == Status::Invalid);
}

TEST_CASE(
	"presentation remote routes authenticate hosts and reject replay without consuming refused sequences",
	"[world][presentation]"
) {
	PresentationBus host(11);
	PresentationBus driver(12);
	const auto source = Open(host, "source");
	const auto destination = Open(driver, "destination");
	REQUIRE(driver.RegisterRemote(Name("host.a"), source) == Status::Ok);
	REQUIRE(host.RegisterRemote(Name("driver"), destination) == Status::Ok);
	CHECK(driver.Send(source, destination, 0, {}) == Status::WrongHost);
	REQUIRE(host.Send(source, destination, 72, {}) == Status::Ok);
	auto outbound = host.TakeOutbound();
	REQUIRE(outbound.size() == 1);
	CHECK(outbound[0].Host == Name("driver"));
	CHECK(driver.Receive(Name("host.b"), outbound[0].Message) == Status::WrongHost);
	REQUIRE(driver.Receive(Name("host.a"), outbound[0].Message) == Status::Ok);
	CHECK(driver.Receive(Name("host.a"), outbound[0].Message) == Status::Duplicate);
	auto spoof = outbound[0].Message;
	spoof.From = destination;
	CHECK(driver.Receive(Name("host.a"), spoof) == Status::WrongHost);
	spoof.From = source;
	spoof.From.Session++;
	CHECK(driver.Receive(Name("host.a"), spoof) == Status::StaleEndpoint);
	CHECK(driver.Take(destination).size() == 1);
	REQUIRE(driver.Send(destination, source, 72, {}) == Status::Ok);
	outbound = driver.TakeOutbound();
	REQUIRE(outbound.size() == 1);
	REQUIRE(host.ReceiveFromDriver(outbound[0].Message) == Status::Ok);
	CHECK(host.Take(source).size() == 1);
}

TEST_CASE(
	"presentation codecs reject truncation and invalid identities transactionally", "[world][presentation]"
) {
	PresentationBus bus(55);
	const auto source = Open(bus, "source");
	const auto destination = Open(bus, "destination");
	REQUIRE(bus.Send(source, destination, 999, std::vector<std::byte>(137, std::byte{0xAC})) == Status::Ok);
	const auto message = bus.Take(destination).front();
	core::ByteWriter writer;
	REQUIRE(WritePresentationMessage(writer, message));
	for (size_t length = 0; length < writer.Size(); length++) {
		core::ByteReader reader(writer.Bytes().first(length));
		PresentationMessage previous = message;
		previous.Correlation = 7;
		CHECK_FALSE(ReadPresentationMessage(reader, previous));
		CHECK(reader.Failed());
		CHECK(previous.Correlation == 7);
	}
	core::ByteReader reader(writer.Bytes());
	PresentationMessage read;
	REQUIRE(ReadPresentationMessage(reader, read));
	CHECK(read.From == source);
	CHECK(read.To == destination);
	CHECK(read.Payload == message.Payload);
	CHECK(reader.Remaining() == 0);
	const size_t before = writer.Size();
	read.From.Session = 0;
	CHECK_FALSE(WritePresentationMessage(writer, read));
	CHECK(writer.Size() == before);
	read = message;
	read.To.Channel = std::string(MAX_PRESENTATION_NAME + 1, 'x');
	CHECK_FALSE(WritePresentationMessage(writer, read));
	read = message;
	read.Payload.resize(MAX_PRESENTATION_PAYLOAD + 1);
	CHECK_FALSE(WritePresentationMessage(writer, read));
	CHECK(writer.Size() == before);
	HostFrame frame;
	frame.Signal = HostSignal::Presentation;
	frame.Presentation = message;
	core::ByteWriter framed;
	WriteHostFrame(framed, frame);
	framed.WriteUInt8(0);
	core::ByteReader trailing(framed.Bytes());
	CHECK_FALSE(ReadHostFrame(trailing, frame));
}

TEST_CASE(
	"presentation backpressure preserves retryable sequence and host poll bounds", "[world][presentation]"
) {
	PresentationLimits limits;
	limits.Messages = 1;
	PresentationBus sender(61);
	PresentationBus receiver(62, limits);
	const auto from = Open(sender, "sender");
	const auto to = Open(receiver, "receiver");
	REQUIRE(sender.RegisterRemote(Name("driver"), to) == Status::Ok);
	REQUIRE(receiver.RegisterRemote(Name("sender-host"), from) == Status::Ok);
	REQUIRE(sender.Send(from, to, 1, {}) == Status::Ok);
	REQUIRE(sender.Send(from, to, 2, {}) == Status::Ok);
	const auto queued = sender.TakeOutbound();
	REQUIRE(receiver.Receive(Name("sender-host"), queued[0].Message) == Status::Ok);
	CHECK(receiver.Receive(Name("sender-host"), queued[1].Message) == Status::Full);
	CHECK(receiver.Take(to).size() == 1);
	REQUIRE(receiver.Receive(Name("sender-host"), queued[1].Message) == Status::Ok);
	CHECK(receiver.Take(to).front().Correlation == 2);
	CHECK(receiver.Receive(Name("sender-host"), queued[0].Message) == Status::Duplicate);

	auto [first, second] = parallel::MakeLocalChannel();
	HostLink writer(std::move(first));
	HostLink reader(std::move(second));
	for (unsigned index = 0; index < 70; index++) {
		REQUIRE(writer.SendPresentation(queued[0].Message));
	}
	std::vector<HostFrame> frames;
	CHECK(reader.Receive(frames) == 64);
	CHECK(frames.size() == 64);
	frames.clear();
	CHECK(reader.Receive(frames) == 6);
	CHECK(reader.Malformed() == 0);

	PresentationMessage oversized = queued[0].Message;
	oversized.Payload.resize(MAX_PRESENTATION_PAYLOAD + 1);
	CHECK_FALSE(writer.SendPresentation(oversized));
	CHECK(writer.Dropped() == 1);
	frames.clear();
	CHECK(reader.Receive(frames) == 0);
}

TEST_CASE(
	"presentation is separate from replica authority simulation inboxes and snapshots",
	"[world][presentation]"
) {
	Universe universe;
	const WorldId source = universe.Create(Named("source"));
	const WorldId destination = universe.Create(Named("destination"));
	REQUIRE(universe.ConfigurePresentation(400));
	const auto from = universe.OpenPresentation(source, Name("image")).Address;
	const auto to = universe.OpenPresentation(destination, Name("image")).Address;
	universe.Enter(source, [](ecs::Store &store) {
		store.SetResource<Replica>(Replica{});
		CHECK_FALSE(Postbox(store).SendTo("destination", "image", {}).Expected());
	});
	core::ByteWriter before;
	REQUIRE(universe.Save(before));
	REQUIRE(universe.SendPresentation(source, from, to, 1, std::vector<std::byte>(11)) == Status::Ok);
	CHECK(universe.SendPresentation(destination, from, to, 1, {}) == Status::WrongHost);
	core::ByteWriter after;
	REQUIRE(universe.Save(after));
	CHECK(std::ranges::equal(before.Bytes(), after.Bytes()));
	CHECK(universe.PresentationQueueUsage().Bytes == 11);
	CHECK(universe.PresentationTrafficCounts().EnqueuedBytes == 11);
	universe.Enter(destination, [](ecs::Store &store) { CHECK(Postbox(store).Deliveries().empty()); });
	REQUIRE(universe.Destroy(destination) == WorldStatus::Ok);
	CHECK(universe.PresentationQueueUsage().Messages == 0);
	const auto recreated = universe.Create(Named("destination"));
	const auto newTo = universe.OpenPresentation(recreated, Name("image")).Address;
	CHECK(newTo.Generation != to.Generation);
	CHECK(universe.SendPresentation(source, from, to, 2, {}) == Status::StaleEndpoint);
	REQUIRE(universe.SendPresentation(source, from, newTo, 3, {}) == Status::Ok);
	core::ByteReader restore(before.Bytes());
	REQUIRE(universe.Load(restore));
	CHECK(universe.PresentationQueueUsage().Messages == 0);
	CHECK(universe.TakePresentation(newTo).empty());
}

TEST_CASE(
	"driver pumps presentation by authenticated link outside simulation ticks", "[world][presentation]"
) {
	Driver driver;
	driver.Hosts().SetLauncher([](const HostPlan &, parallel::Process &) { return true; });
	REQUIRE(driver.Start({Named("remote")}) == 1);
	const auto remote = driver.Worlds().Find(Name("remote"));
	const auto hostName = driver.Worlds().HostOf(remote);
	auto [local, peer] = parallel::MakeLocalChannel();
	REQUIRE(driver.Hosts().Attach(hostName, std::move(local)));
	HostLink link(std::move(peer), Name("forged-frame-host"));
	const auto destination = driver.Worlds().Create(Named("destination"));
	REQUIRE(driver.Worlds().ConfigurePresentation(30));
	const auto to = driver.Worlds().OpenPresentation(destination, Name("image")).Address;
	PresentationBus host(31);
	const auto from = Open(host, "remote");
	CHECK(driver.Worlds().RegisterRemotePresentation(Name("wrong-host"), from) == Status::WrongHost);
	HostFrame advertisement;
	advertisement.Signal = HostSignal::PresentationDirectory;
	advertisement.Directory = host.LocalDirectory();
	REQUIRE(link.Send(advertisement));
	CHECK(driver.PumpPresentation(0).Refused == 0);
	std::vector<HostFrame> routes;
	REQUIRE(Await(link, routes));
	REQUIRE(routes.size() == 1);
	REQUIRE(routes.front().Signal == HostSignal::PresentationRoutes);
	REQUIRE(host.ApplyRoutesFromDriver(routes.front().Directory) == Status::Ok);
	CHECK(host.Lookup("destination", "image") == to);
	REQUIRE(host.Send(from, to, 44, std::vector<std::byte>(53)) == Status::Ok);
	const auto traffic = host.TakeOutbound();
	REQUIRE(link.SendPresentation(traffic[0].Message));
	const auto result = driver.PumpPresentation(0);
	CHECK(result.Accepted == 1);
	CHECK(result.AcceptedPayloadBytes == 53);
	CHECK(result.Refused == 0);
	CHECK(driver.Worlds().LookupPresentation(remote, "image") == from);
	CHECK(driver.Worlds().TakePresentation(to).size() == 1);
	REQUIRE(
		driver.Worlds().SendPresentation(destination, to, from, 44, std::vector<std::byte>(17)) == Status::Ok
	);
	const auto sent = driver.PumpPresentation(0);
	CHECK(sent.Sent == 1);
	CHECK(sent.SentPayloadBytes == 17);
	std::vector<HostFrame> replies;
	REQUIRE(Await(link, replies));
	REQUIRE(replies.size() == 1);
	REQUIRE(host.ReceiveFromDriver(replies[0].Presentation) == Status::Ok);
	CHECK(host.Take(from).size() == 1);
	auto spoof = traffic[0].Message;
	spoof.From = to;
	REQUIRE(link.SendPresentation(spoof));
	CHECK(driver.PumpPresentation(0).Refused == 1);
	REQUIRE(host.Close(from) == Status::Ok);
	HostFrame withdrawal;
	withdrawal.Signal = HostSignal::PresentationDirectory;
	withdrawal.Directory = host.LocalDirectory();
	REQUIRE(link.Send(withdrawal));
	CHECK(driver.PumpPresentation(0).Refused == 0);
	CHECK(driver.Worlds().LookupPresentation(remote, "image").World.empty());
	REQUIRE(link.Send(advertisement));
	CHECK(driver.PumpPresentation(0).Refused == 1);
	CHECK(driver.Worlds().LookupPresentation(remote, "image").World.empty());
}

TEST_CASE("presentation process child", "[.presentation-child]") {
	auto channel = parallel::AdoptInheritedChannel();
	REQUIRE(channel != nullptr);
	HostLink link(std::move(channel));
	PresentationBus host(2);
	const auto destination = Open(host, "child");
	const PresentationAddress source{"parent", "image", 1, 1};
	REQUIRE(host.RegisterRemote(Name("driver"), source) == Status::Ok);
	std::vector<HostFrame> frames;
	REQUIRE(Await(link, frames));
	REQUIRE(frames.size() == 1);
	REQUIRE(host.ReceiveFromDriver(frames[0].Presentation) == Status::Ok);
	const auto delivered = host.Take(destination);
	REQUIRE(delivered.size() == 1);
	REQUIRE(delivered[0].Payload.size() == 300000);
	REQUIRE(host.Send(destination, source, delivered[0].Correlation, delivered[0].Payload) == Status::Ok);
	const auto outgoing = host.TakeOutbound();
	REQUIRE(link.SendPresentation(outgoing[0].Message));
	// Send queues bytes; service the channel until the parent owns the reply.
	frames.clear();
	REQUIRE(Await(link, frames));
	REQUIRE(host.ReceiveFromDriver(frames[0].Presentation) == Status::Ok);
	const auto acknowledged = host.Take(destination);
	REQUIRE(acknowledged.size() == 1);
	CHECK(acknowledged[0].Payload.empty());
}

TEST_CASE("presentation bytes cross a real process with the same endpoint checks", "[world][presentation]") {
	auto pair = parallel::MakeProcessChannel();
	REQUIRE(pair.Valid());
	parallel::Process child;
	REQUIRE(child.Start(
		core::Paths::Base() / core::Paths::Program("test_world"),
		{"presentation process child"},
		std::move(pair.Remote)
	));
	HostLink link(std::move(pair.Local));
	PresentationBus bus(1);
	const auto source = Open(bus, "parent");
	const PresentationAddress destination{"child", "image", 2, 1};
	REQUIRE(bus.RegisterRemote(Name("child-host"), destination) == Status::Ok);
	std::vector<std::byte> bytes(300000);
	for (size_t index = 0; index < bytes.size(); index++) {
		bytes[index] = static_cast<std::byte>(index % 251);
	}
	REQUIRE(bus.Send(source, destination, 718, bytes) == Status::Ok);
	const auto outgoing = bus.TakeOutbound();
	REQUIRE(link.SendPresentation(outgoing[0].Message));
	std::vector<HostFrame> frames;
	REQUIRE(Await(link, frames));
	REQUIRE(frames.size() == 1);
	REQUIRE(bus.Receive(Name("child-host"), frames[0].Presentation) == Status::Ok);
	const auto reply = bus.Take(source);
	REQUIRE(reply.size() == 1);
	CHECK(reply[0].Payload == bytes);
	CHECK(reply[0].Correlation == 718);
	REQUIRE(bus.Send(source, destination, 719, {}) == Status::Ok);
	const auto acknowledgment = bus.TakeOutbound();
	REQUIRE(link.SendPresentation(acknowledgment[0].Message));
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	auto ended = child.Poll();
	while (ended.Alive() && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		ended = child.Poll();
	}
	REQUIRE_FALSE(ended.Alive());
	CHECK(ended.Reason == parallel::ExitReason::Exited);
	CHECK(ended.Code == 0);
}

TEST_CASE("presentation lookup returns only current admitted endpoints", "[world][presentation]") {
	PresentationBus bus(1);
	CHECK(bus.Lookup("far", "image").World.empty());
	const PresentationAddress remote{"far", "image", 2, 1};
	REQUIRE(bus.RegisterRemote(Name("far-host"), remote) == Status::Ok);
	CHECK(bus.Lookup("far", "image") == remote);
	CHECK(bus.Lookup("far", "other").World.empty());
	CHECK(bus.Lookup("other", "image").World.empty());
	REQUIRE(bus.Close(remote) == Status::Ok);
	CHECK(bus.Lookup("far", "image").World.empty());
	auto replacement = remote;
	replacement.Generation++;
	REQUIRE(bus.RegisterRemote(Name("far-host"), replacement) == Status::Ok);
	CHECK(bus.Lookup("far", "image") == replacement);
	CHECK(bus.Close(remote) == Status::StaleEndpoint);
	CHECK(bus.Lookup("far", "image") == replacement);
	bus.RemoveWorld(Name("far"));
	CHECK(bus.Lookup("far", "image").World.empty());
	const auto local = Open(bus, "near");
	CHECK(bus.Lookup("near", "image") == local);
	REQUIRE(bus.Reset(3));
	CHECK(bus.Lookup("near", "image").World.empty());
}

TEST_CASE(
	"presentation directories withdraw endpoints and refuse stale resurrection", "[world][presentation]"
) {
	PresentationBus source(4), receiver(1);
	const auto endpoint = Open(source, "far");
	const auto initial = source.LocalDirectory();
	REQUIRE(receiver.ApplyDirectory(Name("host"), initial) == Status::Ok);
	CHECK(receiver.Lookup("far", "image") == endpoint);
	REQUIRE(source.Close(endpoint) == Status::Ok);
	const auto removed = source.LocalDirectory();
	CHECK(removed.Endpoints.empty());
	REQUIRE(receiver.ApplyDirectory(Name("host"), removed) == Status::Ok);
	CHECK(receiver.Lookup("far", "image").World.empty());
	CHECK(receiver.ApplyDirectory(Name("host"), initial) == Status::StaleEndpoint);
	CHECK(receiver.Lookup("far", "image").World.empty());
	const auto reopened = Open(source, "far");
	REQUIRE(receiver.ApplyDirectory(Name("host"), source.LocalDirectory()) == Status::Ok);
	CHECK(receiver.Lookup("far", "image") == reopened);
	CHECK(receiver.ApplyDirectory(Name("intruder"), source.LocalDirectory()) == Status::WrongHost);
	CHECK(receiver.Lookup("far", "image") == reopened);
	REQUIRE(source.Reset(5));
	REQUIRE(receiver.ApplyDirectory(Name("host"), source.LocalDirectory()) == Status::Ok);
	CHECK(receiver.ApplyDirectory(Name("host"), removed) == Status::StaleEndpoint);
}

TEST_CASE("directory frames reject truncation and duplicate endpoint identities", "[world][presentation]") {
	HostFrame frame;
	frame.Signal = HostSignal::PresentationDirectory;
	frame.Directory = {2, 3, {{"far", "image", 2, 4}}};
	core::ByteWriter writer;
	WriteHostFrame(writer, frame);
	REQUIRE(writer.Size() > 0);
	HostFrame decoded;
	core::ByteReader reader(writer.Bytes());
	REQUIRE(ReadHostFrame(reader, decoded));
	CHECK(decoded.Directory == frame.Directory);
	for (size_t size = 0; size < writer.Size(); ++size) {
		core::ByteReader truncated(std::span(writer.Bytes()).first(size));
		CHECK_FALSE(ReadHostFrame(truncated, decoded));
		CHECK(decoded.Directory == frame.Directory);
	}
	frame.Directory.Endpoints.push_back(frame.Directory.Endpoints.front());
	core::ByteWriter invalid;
	WriteHostFrame(invalid, frame);
	CHECK(invalid.Size() == 0);
	frame.Directory.Endpoints.clear();
	frame.Directory.Endpoints.resize(MAX_PRESENTATION_DIRECTORY + 1);
	WriteHostFrame(invalid, frame);
	CHECK(invalid.Size() == 0);
}

TEST_CASE(
	"directory publication retries a full channel and skips unchanged versions", "[world][presentation]"
) {
	PresentationBus bus(2);
	const auto endpoint = Open(bus, "far");
	const auto initial = bus.LocalDirectory();
	HostFrame advertised;
	advertised.Signal = HostSignal::PresentationDirectory;
	advertised.Directory = initial;
	core::ByteWriter encoded;
	WriteHostFrame(encoded, advertised);
	parallel::ChannelSettings settings;
	settings.Capacity = encoded.Size();
	auto [local, peer] = parallel::MakeLocalChannel(settings);
	HostLink sender(std::move(local)), receiver(std::move(peer));
	HostFrame blocker;
	blocker.Signal = HostSignal::Ready;
	REQUIRE(sender.Send(blocker));
	CHECK_FALSE(sender.PublishPresentationDirectory(initial));
	std::vector<HostFrame> frames;
	REQUIRE(receiver.Receive(frames) == 1);
	CHECK(frames.front().Signal == HostSignal::Ready);
	REQUIRE(sender.PublishPresentationDirectory(initial));
	frames.clear();
	REQUIRE(receiver.Receive(frames) == 1);
	CHECK(frames.front().Directory == initial);
	REQUIRE(sender.PublishPresentationDirectory(initial));
	frames.clear();
	CHECK(receiver.Receive(frames) == 0);
	REQUIRE(bus.Close(endpoint) == Status::Ok);
	REQUIRE(sender.PublishPresentationDirectory(bus.LocalDirectory()));
	REQUIRE(receiver.Receive(frames) == 1);
	CHECK(frames.front().Directory.Endpoints.empty());
	CHECK_FALSE(sender.PublishPresentationDirectory(initial));
	sender.Close();
	CHECK_FALSE(sender.PublishPresentationDirectory(bus.LocalDirectory()));
}

TEST_CASE(
	"driver retires disconnected presentation endpoints before accepting restart", "[world][presentation]"
) {
	Driver driver;
	driver.Hosts().SetLauncher([](const HostPlan &, parallel::Process &) { return true; });
	REQUIRE(driver.Start({Named("remote")}) == 1);
	const auto remote = driver.Worlds().Find(Name("remote"));
	const auto hostName = driver.Worlds().HostOf(remote);
	REQUIRE(driver.Worlds().ConfigurePresentation(1));
	auto [local, peer] = parallel::MakeLocalChannel();
	REQUIRE(driver.Hosts().Attach(hostName, std::move(local)));
	HostLink link(std::move(peer));
	PresentationBus host(2);
	const auto endpoint = Open(host, "remote");
	const auto directory = host.LocalDirectory();
	REQUIRE(link.PublishPresentationDirectory(directory));
	CHECK(driver.PumpPresentation(0).Refused == 0);
	REQUIRE(driver.Worlds().LookupPresentation(remote, "image") == endpoint);
	SECTION("closed link is observed before replacement") {
		link.Close();
		driver.PumpPresentation(0);
		CHECK(driver.Worlds().LookupPresentation(remote, "image").World.empty());
	}
	SECTION("replacement arrives before another pump") {}
	auto [replacement, replacementPeer] = parallel::MakeLocalChannel();
	REQUIRE(driver.Hosts().Attach(hostName, std::move(replacement)));
	HostLink restarted(std::move(replacementPeer));
	REQUIRE(restarted.PublishPresentationDirectory(directory));
	CHECK(driver.PumpPresentation(0).Refused == 1);
	CHECK(driver.Worlds().LookupPresentation(remote, "image").World.empty());
	REQUIRE(host.Reset(3));
	const auto newEndpoint = Open(host, "remote");
	REQUIRE(restarted.PublishPresentationDirectory(host.LocalDirectory()));
	CHECK(driver.PumpPresentation(0).Refused == 0);
	CHECK(driver.Worlds().LookupPresentation(remote, "image") == newEndpoint);
}

TEST_CASE("host retirement purges queued images and retains the session tombstone", "[world][presentation]") {
	PresentationBus bus(1);
	const auto local = Open(bus, "near");
	PresentationDirectory directory{2, 1, {{"far", "image", 2, 1}}};
	const Name host("far-host");
	REQUIRE(bus.ApplyDirectory(host, directory) == Status::Ok);
	REQUIRE(bus.Send(local, directory.Endpoints.front(), 1, std::vector<std::byte>(128)) == Status::Ok);
	CHECK(bus.Queued().Bytes == 128);
	bus.RetireHost(host);
	CHECK(bus.Queued().Bytes == 0);
	CHECK(bus.TakeOutbound().empty());
	CHECK(bus.Lookup("near", "image") == local);
	directory.Revision++;
	CHECK(bus.ApplyDirectory(host, directory) == Status::StaleEndpoint);
	CHECK(bus.RegisterRemote(host, directory.Endpoints.front()) == Status::StaleEndpoint);
	bus.RetireHost({});
	CHECK(bus.Lookup("near", "image") == local);
	directory.Session++;
	directory.Endpoints.front().Session++;
	REQUIRE(bus.ApplyDirectory(host, directory) == Status::Ok);
	CHECK(bus.Lookup("far", "image") == directory.Endpoints.front());
}

TEST_CASE("driver forwards producer routes and withdraws them from consumer hosts", "[world][presentation]") {
	DriverSettings settings;
	settings.Hosts.WorldsPerHost = 1;
	Driver driver(settings);
	driver.Hosts().SetLauncher([](const HostPlan &, parallel::Process &) { return true; });
	REQUIRE(driver.Start({Named("near"), Named("far")}) == 2);
	REQUIRE(driver.Worlds().ConfigurePresentation(1));
	const auto nearHost = driver.Worlds().HostOf(driver.Worlds().Find(Name("near")));
	const auto farHost = driver.Worlds().HostOf(driver.Worlds().Find(Name("far")));
	auto [nearLocal, nearPeer] = parallel::MakeLocalChannel();
	auto [farLocal, farPeer] = parallel::MakeLocalChannel();
	REQUIRE(driver.Hosts().Attach(nearHost, std::move(nearLocal)));
	REQUIRE(driver.Hosts().Attach(farHost, std::move(farLocal)));
	HostLink nearLink(std::move(nearPeer)), farLink(std::move(farPeer));
	PresentationBus near(2), far(3);
	const auto from = Open(near, "near");
	const auto to = Open(far, "far");
	REQUIRE(nearLink.PublishPresentationDirectory(near.LocalDirectory()));
	REQUIRE(farLink.PublishPresentationDirectory(far.LocalDirectory()));
	CHECK(driver.PumpPresentation(0).Refused == 0);
	std::vector<HostFrame> nearFrames, farFrames;
	REQUIRE(Await(nearLink, nearFrames));
	REQUIRE(Await(farLink, farFrames));
	REQUIRE(nearFrames.size() == 1);
	REQUIRE(farFrames.size() == 1);
	REQUIRE(nearFrames[0].Signal == HostSignal::PresentationRoutes);
	REQUIRE(farFrames[0].Signal == HostSignal::PresentationRoutes);
	REQUIRE(near.ApplyRoutesFromDriver(nearFrames[0].Directory) == Status::Ok);
	REQUIRE(far.ApplyRoutesFromDriver(farFrames[0].Directory) == Status::Ok);
	CHECK(near.Lookup("near", "image") == from);
	CHECK(near.Lookup("far", "image") == to);
	CHECK(far.Lookup("near", "image") == from);
	const auto previous = nearFrames[0].Directory;
	REQUIRE(near.Send(from, to, 17, std::vector<std::byte>(128)) == Status::Ok);
	for (const auto &outgoing : near.TakeOutbound())
		REQUIRE(nearLink.SendPresentation(outgoing.Message));
	CHECK(driver.PumpPresentation(0).Sent == 1);
	farFrames.clear();
	REQUIRE(Await(farLink, farFrames));
	REQUIRE(farFrames.size() == 1);
	REQUIRE(far.ReceiveFromDriver(farFrames[0].Presentation) == Status::Ok);
	CHECK(far.Take(to).front().Payload.size() == 128);
	farLink.Close();
	driver.PumpPresentation(0);
	nearFrames.clear();
	REQUIRE(Await(nearLink, nearFrames));
	REQUIRE(nearFrames.size() == 1);
	REQUIRE(near.ApplyRoutesFromDriver(nearFrames[0].Directory) == Status::Ok);
	CHECK(near.Lookup("far", "image").World.empty());
	CHECK(near.ApplyRoutesFromDriver(previous) == Status::StaleEndpoint);
	CHECK(near.Lookup("near", "image") == from);
	nearFrames.clear();
	driver.PumpPresentation(0);
	CHECK(nearLink.Receive(nearFrames) == 0);
}

TEST_CASE(
	"driver route frames preserve producer sessions without widening host advertisements",
	"[world][presentation]"
) {
	HostFrame frame;
	frame.Signal = HostSignal::PresentationRoutes;
	frame.Directory = {1, 7, {{"near", "image", 2, 3}, {"far", "image", 4, 5}}};
	core::ByteWriter writer;
	WriteHostFrame(writer, frame);
	REQUIRE(writer.Size() > 0);
	HostFrame decoded;
	core::ByteReader reader(writer.Bytes());
	REQUIRE(ReadHostFrame(reader, decoded));
	CHECK(decoded.Directory == frame.Directory);
	for (size_t size = 0; size < writer.Size(); ++size) {
		core::ByteReader truncated(std::span(writer.Bytes()).first(size));
		CHECK_FALSE(ReadHostFrame(truncated, decoded));
		CHECK(decoded.Directory == frame.Directory);
	}
	core::ByteWriter owned;
	CHECK_FALSE(WritePresentationDirectory(owned, frame.Directory));
	CHECK(owned.Empty());
	PresentationBus consumer(9);
	const auto local = Open(consumer, "near");
	CHECK(consumer.ApplyRoutesFromDriver(frame.Directory) == Status::WrongHost);
	CHECK(consumer.Lookup("near", "image") == local);
	CHECK(consumer.Lookup("far", "image").World.empty());
}
