// Who gets in, and what it costs to be turned away.
//
// `EndToEnd.cpp` proves a client that behaves reaches a world. This is the
// other half — the cases where it does not behave, which is the half D00006
// existed for. The claims each case is here to hold:
//
// - A stranger's first datagram admits nothing. It used to admit everything.
// - **An unanswered challenge allocates nothing**, and that is asserted against
//   the slot count and the session count rather than against a comment.
// - A forged, replayed or expired cookie is refused.
// - A welcome tampered with in flight is refused rather than half-accepted.
// - `MaximumClients` still turns away the peer past it and still counts it.
// - The game's policy is asked, and the default is exactly what the header
//   says it is.
//
// Every deadline here is stated rather than waited for.

#include <engine/assets/Signature.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/net/Cookie.hpp>
#include <engine/net/Handshake.hpp>
#include <engine/net/Packet.hpp>
#include <engine/net/Transport.hpp>
#include <engine/replication/Admission.hpp>
#include <engine/replication/Connector.hpp>
#include <engine/replication/Listener.hpp>
#include <engine/replication/Session.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.replication.admission")
TEST_DEPENDS("engine.net.cookie")
TEST_DEPENDS("engine.net.handshake")
TEST_DEPENDS("engine.net.packet")

using engine::core::ByteReader;
using engine::core::ByteWriter;
using engine::ecs::Store;
using engine::net::ConnectionId;
using engine::net::Cookie;
using engine::net::Endpoint;
using engine::net::Handshake;
using engine::net::HandshakeRole;
using engine::net::MakeLoopbackTransport;
using engine::net::Transport;
using engine::net::TransportStatus;
using engine::replication::Admission;
using engine::replication::AdmissionKind;
using engine::replication::AdmissionTranscript;
using engine::replication::Answer;
using engine::replication::Applicant;
using engine::replication::Challenge;
using engine::replication::Connector;
using engine::replication::ConnectorSettings;
using engine::replication::FrameAdmission;
using engine::replication::Hello;
using engine::replication::Listener;
using engine::replication::ListenerSettings;
using engine::replication::ReadAdmission;
using engine::replication::Welcome;
using engine::replication::WriteAdmission;

namespace admission_test {

	// Encodes one admission message and puts it on the wire, framed exactly as
	// both real ends frame it.
	template <class T> void Post(Transport &from, const Endpoint &to, const T &message) {
		ByteWriter body;
		WriteAdmission(body, message);

		ByteWriter datagram;
		REQUIRE(FrameAdmission(datagram, body.Bytes()));
		REQUIRE(from.Send(to, datagram.Bytes()) == TransportStatus::Ok);
	}

	// One arrived admission message and who sent it.
	struct Arrival {
		Endpoint From;
		Admission Message;
	};

	// The next admission message waiting on an end, dropping anything that is
	// not one.
	std::optional<Arrival> Take(Transport &end) {
		std::vector<std::byte> datagram;
		while (end.Receive(datagram).Status == TransportStatus::Ok) {
			ByteReader reader(datagram);
			const auto packet = engine::net::Packet::Read(reader);
			if (!packet.has_value() || packet->Header.Channel != engine::net::ChannelKind::Handshake) {
				continue;
			}

			ByteReader body(packet->Payload);
			Arrival arrival;
			if (ReadAdmission(body, arrival.Message)) {
				// The loopback reports the sender, which is the whole reason
				// this suite can tell one stranger from another.
				arrival.From = end.Local();
				return arrival;
			}
		}
		return std::nullopt;
	}

	// The same, keeping the sender the transport reported.
	std::optional<Arrival> TakeFrom(Transport &end) {
		std::vector<std::byte> datagram;
		for (;;) {
			const Transport::Inbound inbound = end.Receive(datagram);
			if (inbound.Status != TransportStatus::Ok) {
				return std::nullopt;
			}

			ByteReader reader(datagram);
			const auto packet = engine::net::Packet::Read(reader);
			if (!packet.has_value() || packet->Header.Channel != engine::net::ChannelKind::Handshake) {
				continue;
			}

			ByteReader body(packet->Payload);
			Arrival arrival;
			arrival.From = inbound.From;
			if (ReadAdmission(body, arrival.Message)) {
				return arrival;
			}
		}
	}

	std::array<std::byte, Handshake::SECRET_BYTES> Seed(uint8_t fill) {
		std::array<std::byte, Handshake::SECRET_BYTES> seed{};
		seed.fill(static_cast<std::byte>(fill));
		return seed;
	}

	// A peer that speaks the exchange by hand, so a suite can send a step that
	// is wrong in exactly one way.
	struct Stranger {
		Transport *Wire = nullptr;
		Endpoint Server;
		std::optional<Handshake> Exchange;
		std::array<std::byte, Handshake::MESSAGE_BYTES> Mine{};
		std::array<std::byte, Cookie::COOKIE_BYTES> Cookie_{};

		Stranger(Transport &wire, const Endpoint &server, uint8_t secret) : Wire(&wire), Server(server) {
			Exchange = Handshake::BeginFromSecret(HandshakeRole::Initiator, Seed(secret));
			REQUIRE(Exchange.has_value());

			const auto mine = Exchange->Message();
			std::copy(mine.begin(), mine.end(), Mine.begin());
		}

		void SayHello() {
			Hello hello;
			hello.PublicKey = Mine;
			Post(*Wire, Server, hello);
		}

		bool TakeCookie() {
			const std::optional<Arrival> arrival = Take(*Wire);
			if (!arrival.has_value() || arrival->Message.Kind != AdmissionKind::Challenge) {
				return false;
			}
			Cookie_ = arrival->Message.Challenge.Cookie;
			return true;
		}

		void SayAnswer() {
			Answer answer;
			answer.PublicKey = Mine;
			answer.Cookie = Cookie_;
			Post(*Wire, Server, answer);
		}

		bool TakeWelcome() {
			const std::optional<Arrival> arrival = Take(*Wire);
			return arrival.has_value() && arrival->Message.Kind == AdmissionKind::Welcome;
		}
	};

	// A signing key from a fixed seed, so a suite gets the same identity twice.
	engine::assets::SigningKey Identity(uint8_t fill) {
		std::array<std::byte, engine::assets::SigningKey::SEED_BYTES> seed{};
		seed.fill(static_cast<std::byte>(fill));

		std::optional<engine::assets::SigningKey> key = engine::assets::SigningKey::FromSeed(seed);
		REQUIRE(key.has_value());
		return std::move(*key);
	}

	// The responder's half, by hand, so a suite can hand a client a welcome
	// that is wrong in exactly one way. Mirrors `Listener::Accept`.
	//
	// **`clientKey` is separate from `answer.PublicKey` and that is the whole
	// point of the parameter.** A relay answers the server with *its own*
	// ephemeral key, so the transcript the server signs names the relay's key
	// and not the client's. Passing them apart is how a suite builds exactly
	// that welcome.
	Welcome Serve(
		const Answer &answer,
		uint8_t secret,
		const engine::assets::SigningKey *identity = nullptr,
		const std::array<std::byte, Handshake::MESSAGE_BYTES> *clientKey = nullptr
	) {
		auto exchange = Handshake::BeginFromSecret(HandshakeRole::Responder, Seed(secret));
		REQUIRE(exchange.has_value());
		REQUIRE(exchange->Consume(answer.PublicKey));

		Welcome welcome;
		const auto mine = exchange->Message();
		std::copy(mine.begin(), mine.end(), welcome.PublicKey.begin());

		auto keys = exchange->TakeKeys();
		REQUIRE(keys.has_value());

		const auto transcript = AdmissionTranscript(
			clientKey != nullptr ? *clientKey : answer.PublicKey, welcome.PublicKey, answer.Cookie
		);
		const auto sealed = keys->Sending.Seal({}, transcript);
		REQUIRE(sealed.has_value());
		REQUIRE(sealed->Bytes.size() == welcome.Confirmation.size());

		welcome.Counter = sealed->Counter;
		std::copy(sealed->Bytes.begin(), sealed->Bytes.end(), welcome.Confirmation.begin());

		if (identity != nullptr) {
			const engine::assets::SignatureBytes signature = identity->SignSessionTranscript(transcript);
			for (size_t index = 0; index < signature.Value.size(); index++) {
				welcome.Identity[index] = static_cast<std::byte>(signature.Value[index]);
			}
		}
		return welcome;
	}

	// A real connector on one end and a hand-written server on the other, run
	// as far as the answer. What is left is the welcome, which is the message
	// each of these cases wants to get wrong.
	struct Dialogue {
		std::vector<std::unique_ptr<Transport>> Transports = MakeLoopbackTransport(2);
		Store Replica{"client"};
		double Now = 0.0;
		std::optional<Connector> Client;
		std::optional<Cookie> Issuer;
		Answer Answered;
		Endpoint ClientAt;
		ConnectorSettings Options;

		explicit Dialogue(const ConnectorSettings &settings = {}) : Options(settings) {
			REQUIRE(Transports.size() == 2);
			Issuer = Cookie::Begin();
			REQUIRE(Issuer.has_value());

			Client.emplace(*Transports[1], Transports[0]->Local(), Now, Options);

			// Hello out, challenge back.
			Client->Poll(Replica, Now);
			const std::optional<Arrival> hello = TakeFrom(*Transports[0]);
			REQUIRE(hello.has_value());
			REQUIRE(hello->Message.Kind == AdmissionKind::Hello);
			ClientAt = hello->From;

			Challenge challenge;
			challenge.Cookie = Issuer->Issue(Now, ClientAt, hello->Message.Hello.PublicKey);
			Post(*Transports[0], ClientAt, challenge);

			// Answer out.
			Client->Poll(Replica, Now);
			const std::optional<Arrival> answered = TakeFrom(*Transports[0]);
			REQUIRE(answered.has_value());
			REQUIRE(answered->Message.Kind == AdmissionKind::Answer);
			REQUIRE(Issuer->Answers(
				Now, ClientAt, answered->Message.Answer.PublicKey, answered->Message.Answer.Cookie
			));
			Answered = answered->Message.Answer;
		}

		// Hands the client a welcome and lets it decide.
		void Finish(const Welcome &welcome) {
			Post(*Transports[0], ClientAt, welcome);
			Client->Poll(Replica, Now);
		}
	};

	// A listener on end zero of a loopback with `peers` other ends beside it.
	struct Port {
		std::vector<std::unique_ptr<Transport>> Transports;
		std::optional<Listener> Server;

		explicit Port(size_t peers, const ListenerSettings &settings = {}) {
			Transports = MakeLoopbackTransport(peers + 1);
			REQUIRE(Transports.size() == peers + 1);
			Server.emplace(*Transports[0], settings);
			REQUIRE(Server->Admitting());
		}

		Endpoint Where() const {
			return Transports[0]->Local();
		}

		Stranger Peer(size_t index, uint8_t secret) {
			return Stranger(*Transports[index + 1], Where(), secret);
		}
	};
}

using namespace admission_test;

// --- the framing -------------------------------------------------------------

TEST_CASE("every admission message survives the round trip", "[replication][admission]") {
	Admission read;

	SECTION("hello") {
		Hello hello;
		hello.PublicKey.fill(std::byte{0x11});

		ByteWriter writer;
		WriteAdmission(writer, hello);
		ByteReader reader(writer.Bytes());
		REQUIRE(ReadAdmission(reader, read));
		CHECK(read.Kind == AdmissionKind::Hello);
		CHECK(read.Hello.PublicKey == hello.PublicKey);
	}

	SECTION("challenge") {
		Challenge challenge;
		challenge.Cookie.fill(std::byte{0x22});

		ByteWriter writer;
		WriteAdmission(writer, challenge);
		ByteReader reader(writer.Bytes());
		REQUIRE(ReadAdmission(reader, read));
		CHECK(read.Kind == AdmissionKind::Challenge);
		CHECK(read.Challenge.Cookie == challenge.Cookie);
	}

	SECTION("answer") {
		Answer answer;
		answer.PublicKey.fill(std::byte{0x33});
		answer.Cookie.fill(std::byte{0x44});

		ByteWriter writer;
		WriteAdmission(writer, answer);
		ByteReader reader(writer.Bytes());
		REQUIRE(ReadAdmission(reader, read));
		CHECK(read.Kind == AdmissionKind::Answer);
		CHECK(read.Answer.PublicKey == answer.PublicKey);
		CHECK(read.Answer.Cookie == answer.Cookie);
	}

	SECTION("welcome") {
		Welcome welcome;
		welcome.PublicKey.fill(std::byte{0x55});
		welcome.Counter = 0x0123456789ABCDEFull;
		welcome.Confirmation.fill(std::byte{0x66});

		ByteWriter writer;
		WriteAdmission(writer, welcome);
		ByteReader reader(writer.Bytes());
		REQUIRE(ReadAdmission(reader, read));
		CHECK(read.Kind == AdmissionKind::Welcome);
		CHECK(read.Welcome.PublicKey == welcome.PublicKey);
		CHECK(read.Welcome.Counter == welcome.Counter);
		CHECK(read.Welcome.Confirmation == welcome.Confirmation);
	}
}

TEST_CASE("a malformed admission message is refused whole", "[replication][admission]") {
	// Read before anything at all is known about the sender, so this is the one
	// parser to be strict rather than forgiving in.
	Hello hello;
	hello.PublicKey.fill(std::byte{0x77});

	ByteWriter writer;
	WriteAdmission(writer, hello);
	const std::vector<std::byte> good(writer.Bytes().begin(), writer.Bytes().end());

	Admission read;

	SECTION("nothing at all") {
		ByteReader reader({});
		CHECK_FALSE(ReadAdmission(reader, read));
	}

	SECTION("one byte short") {
		ByteReader reader(std::span<const std::byte>(good).first(good.size() - 1));
		CHECK_FALSE(ReadAdmission(reader, read));
	}

	SECTION("one byte too long") {
		std::vector<std::byte> longer = good;
		longer.push_back(std::byte{0});
		ByteReader reader(longer);
		CHECK_FALSE(ReadAdmission(reader, read));
	}

	SECTION("a version this build does not speak") {
		std::vector<std::byte> wrong = good;
		wrong[0] = std::byte{0xFE};
		ByteReader reader(wrong);
		CHECK_FALSE(ReadAdmission(reader, read));
	}

	SECTION("a kind outside the enum") {
		std::vector<std::byte> wrong = good;
		wrong[2] = std::byte{0x40};
		ByteReader reader(wrong);
		CHECK_FALSE(ReadAdmission(reader, read));
	}
}

// --- what a stranger costs ----------------------------------------------------

TEST_CASE("a stranger's first datagram admits nothing", "[replication][admission]") {
	// **The whole of D00006's first line.** This used to reserve a slot, build
	// a session and complete the link, on any datagram from any address.
	Port port(1);

	ByteWriter writer;
	engine::net::PacketHeader header;
	header.Channel = engine::net::ChannelKind::Unreliable;
	const std::array<std::byte, 4> payload{};
	REQUIRE(engine::net::Packet::Write(writer, header, payload));
	REQUIRE(port.Transports[1]->Send(port.Where(), writer.Bytes()) == TransportStatus::Ok);

	port.Server->Poll(0.0);

	CHECK(port.Server->Count() == 0);
	CHECK(port.Server->Authority().Count() == 0);
	CHECK(port.Server->Stats().Admitted == 0);
	CHECK(port.Server->Stats().Refused == 1);
}

TEST_CASE("an unanswered challenge allocates nothing", "[replication][admission]") {
	// **The claim D00006 asks to be true and this suite asserts against a real
	// measure of it**: sessions held, and slots reserved in the authority.
	// Two hundred peers say hello and none of them answers, which is three
	// times `MaximumClients` — so a build that reserved anything per hello
	// would show up here as a slot count, and past sixty-four as `Turned`.
	constexpr size_t STRANGERS = 200;
	Port port(STRANGERS);

	std::vector<Stranger> crowd;
	crowd.reserve(STRANGERS);
	for (size_t index = 0; index < STRANGERS; index++) {
		crowd.push_back(port.Peer(index, static_cast<uint8_t>(index + 1)));
		crowd.back().SayHello();
	}

	port.Server->Poll(0.0);

	CHECK(port.Server->Stats().Challenged == STRANGERS);
	CHECK(port.Server->Count() == 0);
	CHECK(port.Server->Authority().Count() == 0);
	CHECK(port.Server->Stats().Admitted == 0);
	CHECK(port.Server->Stats().Turned == 0);
	CHECK(port.Server->Stats().Refused == 0);

	// And every one of them was answered, so the cost really was a datagram
	// each rather than a silent drop that would make the counts above trivial.
	for (Stranger &stranger : crowd) {
		CHECK(stranger.TakeCookie());
	}

	// **Nothing was evicted either**, which is what separates "remembers
	// nothing" from "remembers a bounded number". The first peer to be
	// challenged, two hundred challenges ago, answers and gets in.
	crowd.front().SayAnswer();
	port.Server->Poll(0.0);

	CHECK(port.Server->Count() == 1);
	CHECK(port.Server->Authority().Count() == 1);
	CHECK(crowd.front().TakeWelcome());
}

TEST_CASE("a peer that answers the challenge is admitted", "[replication][admission]") {
	Port port(1);
	Stranger stranger = port.Peer(0, 5);

	stranger.SayHello();
	port.Server->Poll(0.0);
	REQUIRE(stranger.TakeCookie());

	stranger.SayAnswer();
	port.Server->Poll(0.0);

	CHECK(stranger.TakeWelcome());
	CHECK(port.Server->Count() == 1);
	CHECK(port.Server->Authority().Count() == 1);
	CHECK(port.Server->Stats().Admitted == 1);
}

TEST_CASE("a peer that never answers is never admitted", "[replication][admission]") {
	// Hello after hello, for as long as it likes. Each is answered with a
	// cookie and none of them is a slot.
	Port port(1);
	Stranger stranger = port.Peer(0, 6);

	for (int attempt = 0; attempt < 50; attempt++) {
		stranger.SayHello();
		port.Server->Poll(static_cast<double>(attempt));
	}

	CHECK(port.Server->Stats().Challenged == 50);
	CHECK(port.Server->Count() == 0);
	CHECK(port.Server->Authority().Count() == 0);
	CHECK(port.Server->Stats().Admitted == 0);
}

// --- forging and replaying ----------------------------------------------------

TEST_CASE("a forged cookie is refused", "[replication][admission]") {
	Port port(1);
	Stranger stranger = port.Peer(0, 7);

	SECTION("a guess") {
		stranger.Cookie_.fill(std::byte{0xAB});
	}

	SECTION("all zeros, having never asked") {
		stranger.Cookie_.fill(std::byte{0x00});
	}

	SECTION("a real cookie with one byte changed") {
		stranger.SayHello();
		port.Server->Poll(0.0);
		REQUIRE(stranger.TakeCookie());
		stranger.Cookie_[3] = static_cast<std::byte>(static_cast<uint8_t>(stranger.Cookie_[3]) ^ 0x01u);
	}

	stranger.SayAnswer();
	port.Server->Poll(0.0);

	CHECK(port.Server->Count() == 0);
	CHECK(port.Server->Authority().Count() == 0);
	CHECK(port.Server->Stats().Admitted == 0);
	CHECK(port.Server->Stats().Refused == 1);
	CHECK_FALSE(stranger.TakeWelcome());
}

TEST_CASE("an answer replayed from another address is refused", "[replication][admission]") {
	// The property the cookie exists for. One machine gets a cookie honestly;
	// another machine sends the same answer and is refused, because the cookie
	// only verifies against the address it was issued to.
	Port port(2);
	Stranger honest = port.Peer(0, 8);
	Stranger thief = port.Peer(1, 9);

	honest.SayHello();
	port.Server->Poll(0.0);
	REQUIRE(honest.TakeCookie());

	// Same key exchange message, same cookie, different address.
	thief.Mine = honest.Mine;
	thief.Cookie_ = honest.Cookie_;
	thief.SayAnswer();
	port.Server->Poll(0.0);

	CHECK(port.Server->Count() == 0);
	CHECK(port.Server->Stats().Refused == 1);

	// And the peer it was issued to is still able to use it.
	honest.SayAnswer();
	port.Server->Poll(0.0);
	CHECK(port.Server->Count() == 1);
}

TEST_CASE("a cookie answered with a different key is refused", "[replication][admission]") {
	// A relay reading a cookie off the wire and presenting it with an ephemeral
	// key of its own, from the address the cookie was issued to. The cookie
	// covers the key exchange message, so the answer does not verify — and this
	// case is here rather than only in `engine.net.cookie` because what it
	// checks is the *wiring*: that the listener passes the peer's key as the
	// evidence rather than issuing a cookie for the address alone.
	Port port(1);
	Stranger stranger = port.Peer(0, 19);

	stranger.SayHello();
	port.Server->Poll(0.0);
	REQUIRE(stranger.TakeCookie());

	auto relay = Handshake::BeginFromSecret(HandshakeRole::Initiator, Seed(20));
	REQUIRE(relay.has_value());
	const auto theirs = relay->Message();
	std::copy(theirs.begin(), theirs.end(), stranger.Mine.begin());

	stranger.SayAnswer();
	port.Server->Poll(0.0);

	CHECK(port.Server->Count() == 0);
	CHECK(port.Server->Authority().Count() == 0);
	CHECK(port.Server->Stats().Refused == 1);
	CHECK_FALSE(stranger.TakeWelcome());
}

TEST_CASE("a cookie stops being answerable", "[replication][admission]") {
	// Stated rather than waited for: the rotation is driven by the time the
	// caller passes in, so a suite can move a server forty seconds forward
	// between two lines.
	ListenerSettings settings;
	settings.Cookie.RotateEverySeconds = 10.0;

	Port port(1, settings);
	Stranger stranger = port.Peer(0, 10);

	stranger.SayHello();
	port.Server->Poll(0.0);
	REQUIRE(stranger.TakeCookie());

	stranger.SayAnswer();
	port.Server->Poll(40.0);

	CHECK(port.Server->Count() == 0);
	CHECK(port.Server->Stats().Refused == 1);
}

TEST_CASE("a key exchange message this build will not agree with is refused", "[replication][admission]") {
	// A low-order point, which RFC 7748 §6.1 asks implementations to check for
	// and which `net::Handshake` refuses. The cookie is honest, so this is the
	// step *after* return routability failing — and it still costs no slot.
	Port port(1);
	Stranger stranger = port.Peer(0, 11);

	stranger.Mine.fill(std::byte{0x00});
	stranger.SayHello();
	port.Server->Poll(0.0);
	REQUIRE(stranger.TakeCookie());

	stranger.SayAnswer();
	port.Server->Poll(0.0);

	CHECK(port.Server->Count() == 0);
	CHECK(port.Server->Authority().Count() == 0);
	CHECK(port.Server->Stats().Refused == 1);
}

TEST_CASE("a server-to-client message from a client is refused", "[replication][admission]") {
	Port port(1);

	Welcome forged;
	forged.PublicKey.fill(std::byte{0x12});
	Post(*port.Transports[1], port.Where(), forged);

	Challenge alsoForged;
	alsoForged.Cookie.fill(std::byte{0x34});
	Post(*port.Transports[1], port.Where(), alsoForged);

	port.Server->Poll(0.0);

	CHECK(port.Server->Count() == 0);
	CHECK(port.Server->Stats().Refused == 2);
}

// --- the client's half --------------------------------------------------------

TEST_CASE("a connector that gets its welcome is admitted", "[replication][admission]") {
	Dialogue dialogue;
	CHECK_FALSE(dialogue.Client->Admitted());

	dialogue.Finish(Serve(dialogue.Answered, 100));

	CHECK(dialogue.Client->Admitted());
	CHECK_FALSE(dialogue.Client->Rejected());
	CHECK(dialogue.Client->Link().State() == engine::net::ConnectionState::Connected);
}

TEST_CASE("a tampered welcome is refused rather than half-accepted", "[replication][admission]") {
	// **The reason the welcome carries a tag at all.** Without it a rewritten
	// public key is accepted, the two ends derive different keys, and nothing
	// says so — the failure moves to whatever first depends on the keys, which
	// is a long way from the message that caused it.
	Dialogue dialogue;
	Welcome welcome = Serve(dialogue.Answered, 101);

	SECTION("the tag changed") {
		welcome.Confirmation[0] = static_cast<std::byte>(static_cast<uint8_t>(welcome.Confirmation[0]) ^ 1u);
	}

	SECTION("the counter changed") {
		welcome.Counter++;
	}

	SECTION("the public key changed") {
		welcome.PublicKey[0] = static_cast<std::byte>(static_cast<uint8_t>(welcome.PublicKey[0]) ^ 1u);
	}

	SECTION("a whole different server's key") {
		auto other = Handshake::BeginFromSecret(HandshakeRole::Responder, Seed(102));
		REQUIRE(other.has_value());
		const auto mine = other->Message();
		std::copy(mine.begin(), mine.end(), welcome.PublicKey.begin());
	}

	dialogue.Finish(welcome);

	CHECK_FALSE(dialogue.Client->Admitted());
	CHECK(dialogue.Client->Rejected());
	CHECK(dialogue.Client->Link().State() == engine::net::ConnectionState::Disconnected);
	CHECK(dialogue.Client->Link().Reason() == engine::net::DisconnectReason::HandshakeFailed);
}

TEST_CASE("a welcome bound to another cookie does not verify", "[replication][admission]") {
	// The cookie is in the transcript, so a welcome that agreed with a
	// different one is a welcome for a different exchange.
	Dialogue dialogue;

	Answer elsewhere = dialogue.Answered;
	elsewhere.Cookie[0] = static_cast<std::byte>(static_cast<uint8_t>(elsewhere.Cookie[0]) ^ 1u);
	dialogue.Finish(Serve(elsewhere, 103));

	CHECK_FALSE(dialogue.Client->Admitted());
	CHECK(dialogue.Client->Rejected());
}

TEST_CASE("a connector sends nothing of the world before it is admitted", "[replication][admission]") {
	// The link refuses payload while it is `Connecting`, and this is the case
	// that says the connector does not try to route around that.
	Dialogue dialogue;

	Store scratch("client");
	CHECK_FALSE(dialogue.Client->Submit(1, {}, dialogue.Now));
	CHECK_FALSE(dialogue.Client->Joined());
}

TEST_CASE("a client whose exchange goes unanswered gives up", "[replication][admission]") {
	// **Stated, not waited for.** `LinkSettings::HandshakeTimeoutSeconds` is the
	// deadline and this moves a clock past it in one line, which is the whole
	// reason time is passed in.
	std::vector<std::unique_ptr<Transport>> transports = MakeLoopbackTransport(2);
	REQUIRE(transports.size() == 2);

	Store replica("client");
	Connector client(*transports[1], transports[0]->Local(), 0.0);

	// Nobody is listening on end zero, so the hello goes nowhere.
	for (int tick = 0; tick < 8; tick++) {
		const double now = static_cast<double>(tick) * 0.25;
		client.Poll(replica, now);
		client.Advance(now);
	}

	CHECK_FALSE(client.Admitted());
	CHECK_FALSE(client.Rejected());
	CHECK(client.Link().State() == engine::net::ConnectionState::Connecting);

	client.Advance(6.0);
	CHECK(client.Link().State() == engine::net::ConnectionState::Disconnected);
	CHECK(client.Link().Reason() == engine::net::DisconnectReason::HandshakeFailed);

	// And it falls silent rather than repeating into a link that has ended.
	std::vector<std::byte> scratch;
	while (transports[0]->Receive(scratch).Status == TransportStatus::Ok) {}

	client.Poll(replica, 6.5);
	CHECK(transports[0]->Receive(scratch).Status != TransportStatus::Ok);
	CHECK(client.Rejected());
}

// --- the bound, and the policy -----------------------------------------------

TEST_CASE("the bound still turns away the peer past it", "[replication][admission]") {
	// **`MaximumClients` is defence the handshake sits in front of, not defence
	// the handshake replaces.** Every one of these peers completes the exchange
	// honestly; the sixty-fifth is still refused and still counted.
	ListenerSettings settings;
	settings.MaximumClients = 64;

	Port port(65, settings);

	std::vector<Stranger> crowd;
	crowd.reserve(65);
	for (size_t index = 0; index < 65; index++) {
		crowd.push_back(port.Peer(index, static_cast<uint8_t>(index + 1)));
	}

	for (Stranger &stranger : crowd) {
		stranger.SayHello();
	}
	port.Server->Poll(0.0);

	for (Stranger &stranger : crowd) {
		REQUIRE(stranger.TakeCookie());
		stranger.SayAnswer();
	}
	port.Server->Poll(0.0);

	CHECK(port.Server->Count() == 64);
	CHECK(port.Server->Authority().Count() == 64);
	CHECK(port.Server->Stats().Admitted == 64);
	CHECK(port.Server->Stats().Turned == 1);
}

TEST_CASE("the default admits anybody who completes the handshake", "[replication][admission]") {
	// **Asserted because the header states it, and a stated security property
	// that nothing checks is a claim rather than a behaviour.** Completing the
	// exchange proves the peer can receive where it says it can and can do
	// X25519. It proves nothing about who it is, and with no policy set that is
	// enough to get in.
	Port port(1);
	CHECK(port.Server->Stats().Rejected == 0);

	Stranger anybody = port.Peer(0, 12);
	anybody.SayHello();
	port.Server->Poll(0.0);
	REQUIRE(anybody.TakeCookie());
	anybody.SayAnswer();
	port.Server->Poll(0.0);

	CHECK(port.Server->Count() == 1);
	CHECK(port.Server->Stats().Rejected == 0);
}

TEST_CASE("the policy refuses a peer the game rejects", "[replication][admission]") {
	Port port(2);

	Stranger welcome = port.Peer(0, 13);
	Stranger unwanted = port.Peer(1, 14);

	// The game's answer, and the engine has none of its own. Everything the
	// policy is given is here: a proven address, the count, and the time.
	const Endpoint banned = port.Transports[2]->Local();
	size_t asked = 0;
	port.Server->SetAdmission([banned, &asked](const Applicant &applicant) {
		asked++;
		CHECK(applicant.NowSeconds == 3.0);
		return !(applicant.From == banned);
	});

	for (Stranger *stranger : {&welcome, &unwanted}) {
		stranger->SayHello();
	}
	port.Server->Poll(3.0);

	for (Stranger *stranger : {&welcome, &unwanted}) {
		REQUIRE(stranger->TakeCookie());
		stranger->SayAnswer();
	}
	port.Server->Poll(3.0);

	CHECK(asked == 2);
	CHECK(port.Server->Count() == 1);
	CHECK(port.Server->Authority().Count() == 1);
	CHECK(port.Server->Stats().Admitted == 1);
	CHECK(port.Server->Stats().Rejected == 1);

	// Refused in silence. Telling a stranger why it was turned away is telling
	// it what to change.
	CHECK_FALSE(unwanted.TakeWelcome());
	CHECK(welcome.TakeWelcome());
}

TEST_CASE("a rejected peer costs no slot", "[replication][admission]") {
	// The policy is asked before the agreement and before the slot, so a server
	// with a ban list is not paying an X25519 per banned datagram either.
	Port port(1);
	port.Server->SetAdmission([](const Applicant &) { return false; });

	Stranger stranger = port.Peer(0, 15);
	stranger.SayHello();
	port.Server->Poll(0.0);
	REQUIRE(stranger.TakeCookie());
	stranger.SayAnswer();
	port.Server->Poll(0.0);

	CHECK(port.Server->Count() == 0);
	CHECK(port.Server->Authority().Count() == 0);
	CHECK(port.Server->Stats().Rejected == 1);
	CHECK(port.Server->Stats().Refused == 0);
}

// --- after the exchange -------------------------------------------------------

TEST_CASE("a welcome that was lost is sent again", "[replication][admission]") {
	// The responder keeps nothing for a peer that has not answered, so the
	// initiator is the only thing that can cover a loss — including a loss of
	// the welcome itself, which arrives *after* the peer has been admitted.
	Port port(1);
	Stranger stranger = port.Peer(0, 16);

	stranger.SayHello();
	port.Server->Poll(0.0);
	REQUIRE(stranger.TakeCookie());

	stranger.SayAnswer();
	port.Server->Poll(0.0);
	REQUIRE(stranger.TakeWelcome());
	REQUIRE(port.Server->Count() == 1);

	// The client never saw it, so it says the same thing again.
	stranger.SayAnswer();
	port.Server->Poll(0.0);

	CHECK(stranger.TakeWelcome());

	// **And it is the same connection, not a second one.** A fresh agreement
	// for a live peer would be somebody at that address taking the slot from
	// whoever holds it.
	CHECK(port.Server->Count() == 1);
	CHECK(port.Server->Stats().Admitted == 1);
}

TEST_CASE("a fresh hello from an admitted peer is refused", "[replication][admission]") {
	Port port(1);
	Stranger stranger = port.Peer(0, 17);

	stranger.SayHello();
	port.Server->Poll(0.0);
	REQUIRE(stranger.TakeCookie());
	stranger.SayAnswer();
	port.Server->Poll(0.0);
	REQUIRE(stranger.TakeWelcome());

	stranger.SayHello();
	port.Server->Poll(0.0);

	CHECK(port.Server->Count() == 1);
	CHECK(port.Server->Stats().Refused == 1);
	CHECK_FALSE(stranger.TakeCookie());
}

TEST_CASE("a different peer at an admitted address is refused", "[replication][admission]") {
	// Same address, different key exchange message. Either the client restarted
	// and the old session has not timed out, or somebody is trying to displace
	// it — and answering with the welcome that belongs to the live connection
	// would tell them what they wanted to know.
	Port port(1);
	Stranger stranger = port.Peer(0, 18);

	stranger.SayHello();
	port.Server->Poll(0.0);
	REQUIRE(stranger.TakeCookie());
	stranger.SayAnswer();
	port.Server->Poll(0.0);
	REQUIRE(stranger.TakeWelcome());

	stranger.Mine.fill(std::byte{0x99});
	stranger.SayAnswer();
	port.Server->Poll(0.0);

	CHECK(port.Server->Count() == 1);
	CHECK(port.Server->Stats().Admitted == 1);
	CHECK(port.Server->Stats().Refused == 1);
	CHECK_FALSE(stranger.TakeWelcome());
}

TEST_CASE("a session refuses a handshake packet outright", "[replication][admission]") {
	// The other side of the same rule, one layer down. A session exists because
	// an admission already succeeded, so a key exchange arriving on it must not
	// even reach the link — letting it would let whoever sent it reset the idle
	// timeout of a connection they do not own.
	std::vector<std::unique_ptr<Transport>> transports = MakeLoopbackTransport(2);
	REQUIRE(transports.size() == 2);

	engine::replication::Session session(*transports[0], transports[1]->Local(), ConnectionId{1, 1}, 0.0);
	REQUIRE(session.Link().CompleteHandshake(0.0));

	Hello hello;
	hello.PublicKey.fill(std::byte{0x21});

	ByteWriter body;
	WriteAdmission(body, hello);
	ByteWriter datagram;
	REQUIRE(FrameAdmission(datagram, body.Bytes()));

	CHECK_FALSE(session.Receive(datagram.Bytes(), 1.0));
	CHECK(session.Stats().Refused == 1);
	CHECK(session.Link().Stats().PacketsReceived == 0);
}

// --- who the server is --------------------------------------------------------
//
// **The gap `net::Handshake` has carried since v0.3, and what closes it.** An
// X25519 agreement proves the peer can do arithmetic; the cookie proves it can
// receive at the address it wrote; the welcome's tag proves it reached the same
// keys. None of the three says *who* it is — and a relay satisfies all three,
// because it reaches the keys by holding one exchange with each side.
//
// A signature over the transcript is what a relay cannot forge, and these cases
// are the difference between having that sentence in a comment and having it be
// true.

TEST_CASE("a signed welcome from the pinned server is accepted", "[replication][admission]") {
	const engine::assets::SigningKey server = Identity(0x40);

	ConnectorSettings pinned;
	pinned.ServerIdentity = server.Public();

	Dialogue dialogue(pinned);
	dialogue.Finish(Serve(dialogue.Answered, 110, &server));

	CHECK(dialogue.Client->Admitted());
	CHECK_FALSE(dialogue.Client->Rejected());
}

TEST_CASE("an unsigned welcome is refused by a client that pinned a key", "[replication][admission]") {
	// **Refused rather than downgraded.** A client that pinned a key and then
	// connected to a server that could not prove it would have a setting that
	// looks like security and is not — which is worse than not having the
	// setting, because somebody would rely on it.
	const engine::assets::SigningKey server = Identity(0x41);

	ConnectorSettings pinned;
	pinned.ServerIdentity = server.Public();

	Dialogue dialogue(pinned);

	// The welcome a listener with no identity sends: sixty-four zeroes, which
	// verify under no key.
	dialogue.Finish(Serve(dialogue.Answered, 111));

	CHECK_FALSE(dialogue.Client->Admitted());
	CHECK(dialogue.Client->Rejected());
}

TEST_CASE("a welcome signed by another key is refused", "[replication][admission]") {
	const engine::assets::SigningKey expected = Identity(0x42);
	const engine::assets::SigningKey other = Identity(0x43);

	ConnectorSettings pinned;
	pinned.ServerIdentity = expected.Public();

	Dialogue dialogue(pinned);
	dialogue.Finish(Serve(dialogue.Answered, 112, &other));

	CHECK_FALSE(dialogue.Client->Admitted());
	CHECK(dialogue.Client->Rejected());
}

TEST_CASE("a relay in the path is refused, which is the whole point", "[replication][admission]") {
	// **The attack the tag alone cannot catch, modelled the way it actually
	// works.** A relay runs *two* exchanges: one with the client, one with the
	// server. Each half is internally consistent, so each end's confirmation
	// tag verifies — the client's against keys it shares with the relay, the
	// server's against keys it shares with the relay — and the relay reads
	// everything in between.
	//
	// A first attempt at this case had the relay simply forward the server's
	// welcome, and it was refused at the *tag* rather than at the signature —
	// which proves nothing about the signature, because that is a relay too
	// incompetent to be worth defending against. What follows is the competent
	// one: the tag passes and only the signature stands in the way.
	const engine::assets::SigningKey server = Identity(0x44);

	ConnectorSettings pinned;
	pinned.ServerIdentity = server.Public();

	Dialogue dialogue(pinned);

	// The relay's half with the client: it answers as though it were the
	// server, so the client derives keys with *it* and the tag verifies.
	Welcome forwarded = Serve(dialogue.Answered, 113);

	// The relay's half with the server, which it holds separately. The server
	// signs what it saw — a transcript naming the relay's ephemeral key, not
	// the client's — and that signature is the only thing the relay has to
	// offer, because it cannot make the server sign anything else.
	std::array<std::byte, Handshake::MESSAGE_BYTES> relayKey{};
	relayKey.fill(std::byte{0x5A});

	Answer toServer = dialogue.Answered;
	toServer.PublicKey = relayKey;
	const Welcome fromServer = Serve(toServer, 114, &server, &relayKey);

	// The best a relay can do: its own welcome, with the server's real
	// signature stapled on.
	forwarded.Identity = fromServer.Identity;

	dialogue.Finish(forwarded);

	// The tag verified — the client and the relay really do share keys — and
	// the signature did not, because it commits to an exchange this client was
	// not part of.
	CHECK_FALSE(dialogue.Client->Admitted());
	CHECK(dialogue.Client->Rejected());
}

TEST_CASE("without a pin that same relay succeeds", "[replication][admission]") {
	// **The control for the case above, and the reason the pin is not
	// optional in anything that matters.** Identical bytes, one setting
	// removed: the relay gets in, and the client cannot tell.
	const engine::assets::SigningKey server = Identity(0x47);

	Dialogue dialogue;
	Welcome forwarded = Serve(dialogue.Answered, 113);

	std::array<std::byte, Handshake::MESSAGE_BYTES> relayKey{};
	relayKey.fill(std::byte{0x5A});

	Answer toServer = dialogue.Answered;
	toServer.PublicKey = relayKey;
	forwarded.Identity = Serve(toServer, 114, &server, &relayKey).Identity;

	dialogue.Finish(forwarded);
	CHECK(dialogue.Client->Admitted());
}

TEST_CASE("an unpinned client still connects, and that is the weaker mode", "[replication][admission]") {
	// The default, stated rather than hidden: a client with no pinned identity
	// is safe against a listener and not against a relay. `Connector` logs a
	// warning when it starts one of these, so the mode is visible rather than
	// inferred from a setting nobody set.
	Dialogue dialogue;
	dialogue.Finish(Serve(dialogue.Answered, 114));

	CHECK(dialogue.Client->Admitted());

	// And a signature it did not ask for is not a reason to refuse: a signed
	// server talking to an unpinned client is an ordinary deployment.
	const engine::assets::SigningKey server = Identity(0x45);
	Dialogue second;
	second.Finish(Serve(second.Answered, 115, &server));
	CHECK(second.Client->Admitted());
}

TEST_CASE("a signature is over the transcript, so it does not move", "[replication][admission]") {
	// A signature lifted from one exchange verifies in no other, because the
	// transcript names both ephemeral keys and the cookie. This is the same
	// property the tag has, checked for the signature because the two are
	// separate mechanisms over the same bytes.
	const engine::assets::SigningKey server = Identity(0x46);

	ConnectorSettings pinned;
	pinned.ServerIdentity = server.Public();

	Dialogue first(pinned);
	const Welcome stolen = Serve(first.Answered, 116, &server);

	Dialogue second(pinned);
	Welcome forged = Serve(second.Answered, 117, &server);
	forged.Identity = stolen.Identity;

	second.Finish(forged);
	CHECK_FALSE(second.Client->Admitted());
	CHECK(second.Client->Rejected());
}
