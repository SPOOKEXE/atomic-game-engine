// The link that loses things, checked before anything is diagnosed with it.
//
// A wrapper built to find bugs is only worth having if its own behaviour is not
// one of them: a case that fails through a lossy link has to be a case about the
// code underneath, and the first question anybody asks of a failure found this
// way is whether the harness invented it. So this suite states the four things
// every later case leans on — a dropped datagram is not delivered, a seeded drop
// is the same drop twice, nothing is lost when loss is off, and a link with the
// wrapper on it and nothing armed behaves exactly like the link without it.

#include <engine/net/LossyTransport.hpp>
#include <engine/net/Transport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.net.lossytransport")
TEST_DEPENDS("engine.net.transport")

using engine::net::Endpoint;
using engine::net::LossSettings;
using engine::net::LossyTransport;
using engine::net::MakeLoopbackTransport;
using engine::net::Transport;
using engine::net::TransportStatus;

namespace lossy_test {
	std::vector<std::byte> Bytes(std::string_view text) {
		std::vector<std::byte> bytes;
		bytes.reserve(text.size());
		for (const char character : text) {
			bytes.push_back(static_cast<std::byte>(character));
		}
		return bytes;
	}

	// A sender and a receiver, with the loss applied where the datagrams land.
	//
	// The receiving end is the wrapped one, which is the whole shape of the
	// thing: `Send` never lies about what it did, and the datagram is discarded
	// in flight.
	struct Link {
		explicit Link(const LossSettings &settings) {
			std::vector<std::unique_ptr<Transport>> ends = MakeLoopbackTransport(2);
			REQUIRE(ends.size() == 2);

			Sender = std::move(ends[0]);
			Address = ends[1]->Local();
			Receiver = std::make_unique<LossyTransport>(std::move(ends[1]), settings);
		}

		void Say(std::string_view text) {
			REQUIRE(Sender->Send(Address, Bytes(text)) == TransportStatus::Ok);
		}

		// Everything the receiving end will hand over, drained as a caller
		// drains it: until `Empty`.
		std::vector<std::string> Heard() {
			std::vector<std::string> said;
			std::vector<std::byte> datagram;
			while (Receiver->Receive(datagram).Status == TransportStatus::Ok) {
				std::string text;
				for (const std::byte value : datagram) {
					text.push_back(static_cast<char>(value));
				}
				said.push_back(std::move(text));
			}
			return said;
		}

		std::unique_ptr<Transport> Sender;
		std::unique_ptr<LossyTransport> Receiver;
		Endpoint Address;
	};
}

using namespace lossy_test;

TEST_CASE("a nominated datagram does not arrive", "[net][transport][lossy]") {
	LossSettings settings;
	settings.Drop = {1};

	Link link(settings);
	link.Say("first");
	link.Say("second");
	link.Say("third");

	// The nominated one, and only the nominated one.
	CHECK(link.Heard() == std::vector<std::string>{"first", "third"});
	CHECK(link.Receiver->Stats().Dropped == 1);
	CHECK(link.Receiver->Stats().Arrived == 3);
	CHECK(link.Receiver->Stats().Delivered == 2);
}

TEST_CASE("a drop does not stop the drain that follows it", "[net][transport][lossy]") {
	// **The failure mode of a naive wrapper.** A caller polls until `Empty`, so
	// reporting `Empty` for a datagram that was dropped would leave everything
	// behind it in the queue underneath for a tick — which is not loss, it is a
	// stall, and it would show up as a bug in whatever was being tested.
	LossSettings settings;
	settings.Drop = {0, 1};

	Link link(settings);
	link.Say("gone");
	link.Say("also gone");
	link.Say("still here");

	CHECK(link.Heard() == std::vector<std::string>{"still here"});
}

TEST_CASE("the next datagram is dropped whatever number it turns out to have", "[net][transport][lossy]") {
	// The form a case reaches for when it knows what it just made the sender do
	// and not which arrival that will be.
	Link link(LossSettings{});
	link.Say("before");
	CHECK(link.Heard() == std::vector<std::string>{"before"});

	link.Receiver->DropNext(1);
	link.Say("the one that matters");
	link.Say("after");

	CHECK(link.Heard() == std::vector<std::string>{"after"});
	CHECK(link.Receiver->Stats().Dropped == 1);
}

TEST_CASE("a seeded loss is the same loss twice", "[net][transport][lossy]") {
	// The property the whole approach rests on: a failure is reported as a seed
	// and reproduced from it. Two links with one seed lose the same datagrams;
	// a different seed loses a different set, which is what says the seed is
	// being read at all rather than the answer being fixed.
	const auto run = [](uint32_t seed) {
		LossSettings settings;
		settings.LossChance = 0.5f;
		settings.Seed = seed;

		Link link(settings);
		for (int index = 0; index < 40; index++) {
			link.Say(std::to_string(index));
		}
		return link.Heard();
	};

	const std::vector<std::string> first = run(7);
	const std::vector<std::string> again = run(7);
	const std::vector<std::string> other = run(8);

	CHECK(first == again);
	CHECK_FALSE(first.empty());
	CHECK(first.size() < 40);
	CHECK_FALSE(first == other);
}

TEST_CASE("nothing is lost when loss is off", "[net][transport][lossy]") {
	Link link(LossSettings{});
	for (int index = 0; index < 32; index++) {
		link.Say(std::to_string(index));
	}

	CHECK(link.Heard().size() == 32);
	CHECK(link.Receiver->Stats().Dropped == 0);
	CHECK(link.Receiver->Stats().Duplicated == 0);
	CHECK(link.Receiver->Stats().Reordered == 0);
}

TEST_CASE("a link that loses nothing behaves like the link underneath", "[net][transport][lossy]") {
	// Every statement `Transport.hpp` makes about the interface, asked of the
	// wrapper. A case found through a lossy link is only evidence about the code
	// underneath if the wrapper answers the same way when it is not losing
	// anything — and the statuses are the part a wrapper is most likely to get
	// wrong, because they are the part it would have to invent if it dropped on
	// the way out instead.
	std::vector<std::unique_ptr<Transport>> ends = MakeLoopbackTransport(2);
	REQUIRE(ends.size() == 2);

	const Endpoint peer = ends[1]->Local();
	const Endpoint own = ends[0]->Local();
	LossyTransport wrapped(std::move(ends[0]), LossSettings{});

	CHECK(wrapped.Open());
	CHECK(wrapped.Local() == own);

	// `Ok`, `TooLarge` and `Unreachable` are the sender's own business and the
	// wrapper reports what the transport underneath said, unchanged.
	CHECK(wrapped.Send(peer, Bytes("hello")) == TransportStatus::Ok);
	CHECK(
		wrapped.Send(peer, std::vector<std::byte>(Transport::MAXIMUM_DATAGRAM_BYTES + 1, std::byte{0})) ==
		TransportStatus::TooLarge
	);
	CHECK(wrapped.Send(Endpoint{}, Bytes("nowhere")) == TransportStatus::Unreachable);

	// Sending somewhere nobody is listening stays `Ok` and dropped, so
	// single-player still cannot branch on something the real network never
	// says.
	CHECK(wrapped.Send(Endpoint::LoopbackIPv4(9), Bytes("into the void")) == TransportStatus::Ok);

	// The reply comes back through the wrapper, named by its real sender.
	REQUIRE(ends[1]->Send(own, Bytes("and the answer")) == TransportStatus::Ok);
	std::vector<std::byte> datagram;
	const Transport::Inbound inbound = wrapped.Receive(datagram);
	CHECK(inbound.Status == TransportStatus::Ok);
	CHECK(inbound.From == peer);
	CHECK(datagram == Bytes("and the answer"));
	CHECK(wrapped.Receive(datagram).Status == TransportStatus::Empty);

	wrapped.Close();
	CHECK_FALSE(wrapped.Open());
	CHECK_FALSE(wrapped.Local().IsValid());
	CHECK(wrapped.Send(peer, Bytes("too late")) == TransportStatus::Closed);
	CHECK(wrapped.Receive(datagram).Status == TransportStatus::Closed);
}

TEST_CASE("a nominated datagram arrives twice", "[net][transport][lossy]") {
	LossSettings settings;
	settings.Duplicate = {1};

	Link link(settings);
	link.Say("one");
	link.Say("two");
	link.Say("three");

	CHECK(link.Heard() == std::vector<std::string>{"one", "two", "two", "three"});
	CHECK(link.Receiver->Stats().Duplicated == 1);
}

TEST_CASE("a nominated datagram arrives behind the one after it", "[net][transport][lossy]") {
	LossSettings settings;
	settings.Reorder = {1};

	Link link(settings);
	link.Say("one");
	link.Say("two");
	link.Say("three");

	CHECK(link.Heard() == std::vector<std::string>{"one", "three", "two"});
	CHECK(link.Receiver->Stats().Reordered == 1);
}

TEST_CASE("a held datagram is released rather than lost", "[net][transport][lossy]") {
	// A reorder with nothing behind it is a delay of one poll. Holding it for a
	// datagram that may never come would make a nominated reorder an accidental
	// drop, which is the one thing this wrapper must not do by surprise.
	LossSettings settings;
	settings.Reorder = {0};

	Link link(settings);
	link.Say("alone");

	CHECK(link.Heard() == std::vector<std::string>{"alone"});
}
