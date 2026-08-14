// What a sealed stream does, and what it refuses.
//
// `EndToEnd.cpp` and `Loss.cpp` both run over the encrypted stream already —
// `Wire.hpp` hands both sessions real keys from a real `net::Handshake` — so
// those files are the proof that sealing every payload did not break
// replication. This file is the proof that the sealing is real: that the
// plaintext is not on the wire, that a byte changed anywhere in the packet is
// refused rather than half-accepted, that a peer cannot ask for the clear path,
// and that no nonce is used twice.
//
// **The nonce check is not in this file.** It is in `Wire.hpp`'s `Tap`, which
// asserts on every datagram every case in these suites sends. A test that
// sampled a few counters would pass over the one that repeated; the whole point
// of putting the counter on the wire is that a witness can check all of them.
// What this file adds is the assertion that the check has something to check —
// that the run really did produce resends and hundreds of packets — because a
// witness nothing goes past passes for the wrong reason.

#include "Wire.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/net/Cipher.hpp>
#include <engine/net/Handshake.hpp>
#include <engine/net/Packet.hpp>
#include <engine/net/Transport.hpp>
#include <engine/replication/Authority.hpp>
#include <engine/replication/Protocol.hpp>
#include <engine/replication/Session.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.replication.encryption")
TEST_DEPENDS("engine.net.cipher")
TEST_DEPENDS("engine.net.handshake")
TEST_DEPENDS("engine.net.packet")
TEST_DEPENDS("engine.replication.endtoend")

using engine::core::ByteWriter;
using engine::ecs::Entity;
using engine::net::ChannelKind;
using engine::net::Cipher;
using engine::net::ConnectionId;
using engine::net::Handshake;
using engine::net::HandshakeRole;
using engine::net::LossSettings;
using engine::net::Packet;
using engine::net::PacketHeader;
using engine::net::Transport;
using engine::replication::Session;
using namespace replication_wire;

namespace {
	// A byte string nothing else in the protocol would produce.
	constexpr std::string_view CANARY = "PLAINTEXT-CANARY-do-not-ship-this";

	std::vector<std::byte> Bytes(std::string_view text) {
		std::vector<std::byte> data;
		data.reserve(text.size());
		for (const char letter : text) {
			data.push_back(static_cast<std::byte>(letter));
		}
		return data;
	}

	// Whether `haystack` contains `needle` anywhere in it.
	bool Holds(std::span<const std::byte> haystack, std::span<const std::byte> needle) {
		if (needle.empty() || haystack.size() < needle.size()) {
			return false;
		}
		return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) != haystack.end();
	}

	// One input message carrying the canary, encoded.
	std::vector<std::byte> CanaryInput(uint64_t tick) {
		engine::replication::Input input;
		input.Tick = tick;
		input.Bytes = Bytes(CANARY);

		ByteWriter writer;
		WriteMessage(writer, input);
		return {writer.Bytes().begin(), writer.Bytes().end()};
	}
}

TEST_CASE("the payload is not on the wire", "[replication][encryption]") {
	// **Asserted against the bytes the transport was handed, not inferred from
	// the fact that a cipher was called.** A sealing step that quietly passed
	// the plaintext through would satisfy every other case in these suites,
	// because both ends would still agree.
	Wire wire;
	wire.Server.Set<Spot>(wire.Server.Create(), Spot{1.0f, 0.0f});
	REQUIRE(wire.Join());

	const std::vector<std::byte> message = CanaryInput(7);
	const std::vector<std::byte> canary = Bytes(CANARY);

	// The canary is in the plaintext, so the search below is looking for
	// something that was really there.
	REQUIRE(Holds(message, canary));

	REQUIRE(wire.ClientSide->Send(message, wire.Now));

	const std::vector<std::byte> &onWire = wire.ClientTap->Last();
	CHECK_FALSE(Holds(onWire, canary));

	// The tag is on the wire and the header is not encrypted, so the datagram is
	// exactly the header, the message and the sixteen bytes of Poly1305.
	CHECK(onWire.size() == Packet::HEADER_BYTES + message.size() + Cipher::OVERHEAD_BYTES);

	// And it arrives whole at the other end, which is the half that makes the
	// absence above mean encryption rather than corruption.
	wire.Carry(*wire.ServerEnd, *wire.ServerSide);
	REQUIRE(wire.ServerSide->Inbound().size() == 1);
	CHECK(Holds(wire.ServerSide->Inbound()[0], canary));
	CHECK(wire.ServerSide->Stats().Unopened == 0);
}

TEST_CASE("a tampered packet is refused and moves nothing", "[replication][encryption]") {
	// Three places a byte can be changed, and one answer to all three. The
	// second half of each case is the one that matters: a refusal that had
	// already advanced the sequence window would let anybody who can write to
	// this address make the next genuine packet look stale.
	Wire wire;
	wire.Server.Set<Spot>(wire.Server.Create(), Spot{1.0f, 0.0f});
	REQUIRE(wire.Join());

	const std::vector<std::byte> message = CanaryInput(11);
	REQUIRE(wire.ClientSide->Send(message, wire.Now));

	const std::vector<std::byte> genuine = wire.ClientTap->Last();
	REQUIRE(genuine.size() > Packet::HEADER_BYTES);

	// Drained so the only datagram the server sees is the one this case hands
	// it deliberately.
	std::vector<std::byte> discard;
	while (wire.ServerEnd->Receive(discard).Status == engine::net::TransportStatus::Ok) {}

	const uint64_t received = wire.ServerSide->Link().Stats().PacketsReceived;
	uint64_t unopened = 0;

	const auto refuses = [&](size_t index, const char *what) {
		INFO(what);
		std::vector<std::byte> tampered = genuine;
		tampered[index] ^= std::byte{0xFF};

		CHECK_FALSE(wire.ServerSide->Receive(tampered, wire.Now));

		unopened++;
		CHECK(wire.ServerSide->Stats().Unopened == unopened);

		// Nothing about the link moved. Not the packet count, not the sequence
		// window, not the idle clock.
		CHECK(wire.ServerSide->Link().Stats().PacketsReceived == received);
		CHECK(wire.ServerSide->Inbound().empty());
	};

	refuses(Packet::HEADER_BYTES + 2, "a byte of the ciphertext");
	refuses(genuine.size() - 1, "a byte of the tag");
	refuses(9, "the sequence, which is authenticated because the header is the associated data");
	refuses(15, "the nonce counter, which changes the nonce and so fails the tag");

	// **And the genuine packet still opens afterwards.** A refusal that left the
	// sequence window moved, or the opener holding half a frame, would show up
	// here rather than above.
	CHECK(wire.ServerSide->Receive(genuine, wire.Now));
	CHECK(wire.ServerSide->Link().Stats().PacketsReceived == received + 1);
	REQUIRE(wire.ServerSide->Inbound().size() == 1);
	CHECK(Holds(wire.ServerSide->Inbound()[0], Bytes(CANARY)));
}

TEST_CASE("a peer cannot ask for the clear path", "[replication][encryption]") {
	// **The downgrade, and why there is nothing to negotiate.** There is no
	// field on the wire saying whether a packet is sealed, so a peer cannot
	// request the plaintext path — it can only send plaintext and be refused.
	// A receiver that accepted it "because it parsed" would have no encryption
	// at all, whatever the handshake did.
	Wire wire;
	wire.Server.Set<Spot>(wire.Server.Create(), Spot{1.0f, 0.0f});
	REQUIRE(wire.Join());

	std::vector<std::byte> discard;
	while (wire.ServerEnd->Receive(discard).Status == engine::net::TransportStatus::Ok) {}

	// A perfectly well-formed packet. `Packet::Read` accepts it; the session
	// does not.
	ByteWriter body;
	WriteMessage(body, engine::replication::Applied{3});

	PacketHeader header;
	header.Channel = ChannelKind::Reliable;
	header.Sequence = 500;

	ByteWriter datagram;
	REQUIRE(Packet::Write(datagram, header, body.Bytes()));

	const uint64_t received = wire.ServerSide->Link().Stats().PacketsReceived;
	CHECK_FALSE(wire.ServerSide->Receive(datagram.Bytes(), wire.Now));
	CHECK(wire.ServerSide->Stats().Unopened == 1);
	CHECK(wire.ServerSide->Link().Stats().PacketsReceived == received);
	CHECK(wire.ServerSide->Inbound().empty());
}

TEST_CASE("a session with no keys carries nothing", "[replication][encryption]") {
	// Fail closed. A session whose admission did not finish is one that cannot
	// seal, and sending its payload in the clear rather than not at all is the
	// downgrade doing itself.
	std::vector<std::unique_ptr<Transport>> ends = engine::net::MakeLoopbackTransport(2);
	REQUIRE(ends.size() == 2);

	Session bare(*ends[0], ends[1]->Local(), ConnectionId{1, 1}, 0.0);
	REQUIRE(bare.Link().CompleteHandshake(0.0));
	REQUIRE_FALSE(bare.Sealing());

	CHECK_FALSE(bare.Send(CanaryInput(1), 0.0));
	CHECK(bare.Stats().Sent == 0);

	// Nothing left the transport either, which is the assertion that separates
	// "refused" from "sent and not counted".
	std::vector<std::byte> arrived;
	CHECK(ends[1]->Receive(arrived).Status == engine::net::TransportStatus::Empty);

	// And it accepts nothing, including a packet that is otherwise perfect.
	ByteWriter body;
	WriteMessage(body, engine::replication::Applied{1});
	ByteWriter datagram;
	PacketHeader header;
	header.Channel = ChannelKind::Reliable;
	REQUIRE(Packet::Write(datagram, header, body.Bytes()));

	CHECK_FALSE(bare.Receive(datagram.Bytes(), 0.0));
	CHECK(bare.Stats().Unopened == 1);
}

TEST_CASE("a session takes keys once", "[replication][encryption]") {
	// Not idempotent, for the reason `Link::CompleteHandshake` is not. Replacing
	// a live sealer is the one operation here that could put a counter back to a
	// value it has already sealed under.
	std::vector<std::unique_ptr<Transport>> ends = engine::net::MakeLoopbackTransport(2);
	REQUIRE(ends.size() == 2);

	Session first(*ends[0], ends[1]->Local(), ConnectionId{1, 1}, 0.0);
	Session second(*ends[1], ends[0]->Local(), ConnectionId{2, 1}, 0.0);
	Agree(first, second);

	CHECK(first.Sealing());

	std::optional<Handshake> again = Handshake::Begin(HandshakeRole::Responder);
	std::optional<Handshake> other = Handshake::Begin(HandshakeRole::Initiator);
	REQUIRE(again.has_value());
	REQUIRE(other.has_value());
	REQUIRE(again->Consume(other->Message()));

	std::optional<Handshake::Session> spare = again->TakeKeys();
	REQUIRE(spare.has_value());
	CHECK_FALSE(first.AdoptKeys(std::move(*spare)));
}

TEST_CASE("a resend is sealed again rather than replayed", "[replication][encryption]") {
	// **The resend decision, asserted.** `ReliableSender` holds the plaintext and
	// `Session::Flush` seals it again under a fresh counter, so the same reliable
	// sequence goes out twice under two different nonces. The alternative —
	// keeping the sealed bytes and sending them verbatim — would also never
	// repeat a nonce, and would have to either freeze the acknowledgement the
	// resend is carrying or stop covering the header with the tag. This case
	// pins which of the two was chosen.
	Wire wire;
	const Entity mover = wire.Server.Create();
	wire.Server.Set<Spot>(mover, Spot{1.0f, 0.0f});
	REQUIRE(wire.Join());

	// A creation goes out on the reliable channel, so losing it is what makes
	// the sender resend rather than what makes the next tick cover it.
	const Entity late = wire.Server.Create();
	wire.Server.Set<Spot>(late, Spot{9.0f, 9.0f});
	wire.ClientEnd->DropNext(1);

	for (int tick = 0; tick < 60; tick++) {
		wire.Server.GetMutable<Spot>(mover)->X += 1.0f;
		wire.Tick();
	}

	REQUIRE(wire.ClientEnd->Stats().Dropped == 1);
	REQUIRE(wire.Client.Alive(late));
	REQUIRE(wire.ServerSide->Stats().Retransmissions > 0);

	// A reliable sequence that went out more than once, and never twice under
	// one counter.
	size_t repeated = 0;
	const std::span<const Tap::Record> sent = wire.ServerTap->Sent();
	for (size_t index = 0; index < sent.size(); index++) {
		if (sent[index].Channel != ChannelKind::Reliable) {
			continue;
		}
		for (size_t earlier = 0; earlier < index; earlier++) {
			if (sent[earlier].Channel != ChannelKind::Reliable ||
				sent[earlier].Sequence != sent[index].Sequence) {
				continue;
			}
			repeated++;
			CHECK(sent[earlier].Counter != sent[index].Counter);
		}
	}

	CHECK(repeated > 0);
}

TEST_CASE("the stream converges under loss, duplication and reordering", "[replication][encryption]") {
	// **The combination that breaks encryption.** A dropped packet leaves a gap
	// in the counter, a duplicate presents the same counter twice and a reorder
	// presents a lower one after a higher one. An opener that kept any state
	// about the counter — a window, a last-seen, a replay set — would refuse
	// genuine traffic on all three, and the symptom would be a stream that
	// converges everywhere except on a bad network.
	LossSettings toClient;
	toClient.Drop = {7, 19, 31, 43};
	toClient.Duplicate = {3, 4, 5, 11, 12};
	toClient.Reorder = {9, 13, 17, 21};
	toClient.Seed = 90210;

	LossSettings toServer;
	toServer.Drop = {5, 15};
	toServer.Duplicate = {8};
	toServer.Reorder = {6, 10};
	toServer.Seed = 90211;

	Wire wire({}, {}, toClient, toServer);

	std::vector<Entity> all;
	for (int index = 0; index < 20; index++) {
		const Entity entity = wire.Server.Create();
		wire.Server.Set<Spot>(entity, Spot{static_cast<float>(index), 0.0f});
		all.push_back(entity);
	}
	REQUIRE(wire.Join(1024));

	for (int round = 1; round <= 90; round++) {
		for (const Entity entity : all) {
			wire.Server.GetMutable<Spot>(entity)->X = static_cast<float>(round);
		}
		wire.Tick();
	}

	for (const Entity entity : all) {
		REQUIRE(wire.Client.Get<Spot>(entity) != nullptr);
		CHECK(wire.Client.Get<Spot>(entity)->X == 90.0f);
	}

	// The link really did all three things to the traffic.
	CHECK(wire.ClientEnd->Stats().Dropped > 0);
	CHECK(wire.ClientEnd->Stats().Duplicated > 0);
	CHECK(wire.ClientEnd->Stats().Reordered > 0);

	// **And not one genuine packet failed to open.** This is the assertion that
	// says the opener holds no counter state: a duplicate and a reorder are
	// nonces out of order, and out of order is not the same as repeated.
	CHECK(wire.ClientSide->Stats().Unopened == 0);
	CHECK(wire.ServerSide->Stats().Unopened == 0);
	CHECK(wire.Replica_.Stats().Malformed == 0);

	// A duplicate was not applied twice: the world holds twenty entities and no
	// more, and the reliable receiver counted the repeats it dropped.
	CHECK(wire.Client.CountMatching<Spot>() == 20);
}

TEST_CASE("the nonce witness had something to witness", "[replication][encryption]") {
	// `Wire.hpp`'s `Tap` asserts on every datagram in every case in these files,
	// which is worth exactly as much as the number of datagrams that went past
	// it. This is that number, so that a change which stopped the traffic — or
	// stopped the tap seeing it — fails here rather than passing everywhere.
	LossSettings toClient;
	toClient.Drop = {6, 12, 18};

	Wire wire({}, {}, toClient);
	std::vector<Entity> all;
	for (int index = 0; index < 12; index++) {
		const Entity entity = wire.Server.Create();
		wire.Server.Set<Spot>(entity, Spot{static_cast<float>(index), 0.0f});
		all.push_back(entity);
	}
	REQUIRE(wire.Join(512));

	for (int round = 1; round <= 120; round++) {
		for (const Entity entity : all) {
			wire.Server.GetMutable<Spot>(entity)->X = static_cast<float>(round);
		}
		wire.Tick();
	}

	CHECK(wire.ServerTap->Sent().size() > 120);
	CHECK(wire.ClientTap->Sent().size() > 120);

	// The counters are the sealer's own, so the last one a direction sent is at
	// least the number of packets it sent minus one. Stated as a floor rather
	// than an equality because the server's sealer spent nothing on a welcome in
	// this harness and a real one does.
	CHECK(wire.ServerTap->Sent().back().Counter + 1 >= wire.ServerTap->Sent().size());
}

TEST_CASE("a chunk larger than a sealed datagram is capped", "[replication][encryption]") {
	// **The failure this change was most likely to cause.** Sixteen bytes came
	// out of the payload limit, so a chunk size that fitted before may not now —
	// and a message that can never fit is refused by `Link::Reserve`, which is
	// also what an ordinary busy link looks like. The symptom is a client that
	// joins and watches a frozen world, and this module has found it three times
	// already.
	engine::replication::AuthoritySettings settings;
	settings.ChunkBytes = Packet::MAXIMUM_PAYLOAD_BYTES;

	Wire wire({}, settings);

	// **Enough of a world that a chunk is a full one.** A snapshot smaller than
	// `ChunkBytes` fits in one short chunk whatever the setting says, so a
	// handful of entities would pass with the cap taken out — which is a case
	// that cannot fail. Six hundred rows is several full chunks.
	std::vector<Entity> all;
	for (int index = 0; index < 600; index++) {
		const Entity entity = wire.Server.Create();
		wire.Server.Set<Spot>(entity, Spot{static_cast<float>(index), 0.0f});
		all.push_back(entity);
	}

	REQUIRE(wire.Join(512));

	// Every message the authority produced fits, which is what the cap is for.
	for (int round = 1; round <= 20; round++) {
		for (const Entity entity : all) {
			wire.Server.GetMutable<Spot>(entity)->X = static_cast<float>(round);
		}
		wire.Tick();
	}

	for (const Entity entity : all) {
		REQUIRE(wire.Client.Get<Spot>(entity) != nullptr);
		CHECK(wire.Client.Get<Spot>(entity)->X == 20.0f);
	}
	CHECK(wire.ServerSide->Link().Stats().SendsOverBudget == 0);
}

TEST_CASE("the round trip reaches the link with its variance", "[replication]") {
	// **`Session` is the only thing that measures a round trip in this engine,
	// and the congestion controller is the only thing that reads one.** The
	// estimate has crossed since v0.9; what it now has to carry beside it is the
	// variance, because that is what sizes the controller's noise threshold.
	// Passing the estimate alone is not a smaller version of passing both: the
	// threshold falls back to a one-millisecond floor, and a jittery path then
	// reads as a standing queue and the link narrows for something that is not
	// there.
	//
	// RFC 6298 seeds the variance at half the first sample, so this is non-zero
	// after a single measured trip and does not need jitter arranged for it —
	// which is the point, since a case needing an unstable link to see a field
	// arrive would be measuring the link.
	Wire wire;
	wire.Server.Set<Spot>(wire.Server.Create(), Spot{1.0f, 0.0f});
	REQUIRE(wire.Join());

	for (int tick = 0; tick < 30; tick++) {
		wire.Tick();
	}

	// The estimate itself, so a variance of zero is read as "not passed" rather
	// than as "nothing was ever measured".
	// **The client's end, because that is the end whose reliable stream is
	// being acknowledged here.** A round trip is recorded where an
	// acknowledgement lands, and in this exchange the client is the side sending
	// reliably enough to have one closed for it every tick.
	CHECK(wire.ClientSide->Link().Stats().RoundTripMilliseconds > 0.0f);
	CHECK(wire.ClientSide->Link().Stats().RoundTripVarianceMilliseconds > 0.0f);
}
