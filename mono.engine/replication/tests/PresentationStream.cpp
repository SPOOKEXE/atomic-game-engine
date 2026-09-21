#include <engine/assets/Signature.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/net/LossyTransport.hpp>
#include <engine/net/Transport.hpp>
#include <engine/replication/Connector.hpp>
#include <engine/replication/Listener.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/PresentationStream.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

TEST_SUITE_ID("engine.replication.presentationstream")
TEST_DEPENDS("engine.replication.usermessages")
TEST_DEPENDS("engine.replication.quicwire")
TEST_DEPENDS("engine.world.presentationstream")

TEST_CASE(
	"presentation images cross authenticated play connections within packet budgets",
	"[replication][presentation-stream-wire]"
) {
	using namespace engine;
	const auto wire = GENERATE(net::WireMode::Datagram, net::WireMode::Quic);
	const bool losePacket = GENERATE(false, true);
	auto transports = net::MakeLoopbackTransport(2);
	REQUIRE(transports.size() == 2);
	net::LossyTransport clientWire(std::move(transports[1]));
	std::array<std::byte, 32> serverSeed{}, clientSeed{};
	serverSeed.fill(std::byte{23});
	clientSeed.fill(std::byte{45});
	const auto serverKey = assets::SigningKey::FromSeed(serverSeed);
	const auto clientKey = assets::SigningKey::FromSeed(clientSeed);
	REQUIRE(serverKey);
	REQUIRE(clientKey);
	replication::ListenerSettings settings;
	settings.Wire = wire;
	settings.Quic.Connection.Tls.Seed = serverSeed;
	settings.Quic.Connection.Tls.HasSeed = true;
	replication::Listener server(*transports[0], settings);
	server.SetIdentity(&*serverKey);
	server.RequireClientIdentity(true);
	server.SetClientPolicy([&](replication::ClientId, const assets::PublicKey &key) {
		return key == clientKey->Public();
	});
	replication::ConnectorSettings connecting;
	connecting.Advertised = wire;
	connecting.ServerIdentity = serverKey->Public();
	connecting.ClientIdentity = &*clientKey;
	replication::Connector client(clientWire, transports[0]->Local(), 0, connecting);
	world::PresentationStream sending, receiving;
	replication::ClientId peer;
	bool ready = false;
	const std::vector<std::byte> hello{std::byte{19}};
	server.OnUserMessage([&](replication::ClientId id, std::span<const std::byte> bytes) {
		REQUIRE(std::vector<std::byte>(bytes.begin(), bytes.end()) == hello);
		CHECK(server.IdentityOf(id) == clientKey->Public());
		peer = id;
		ready = true;
	});
	client.OnUserMessage([&](std::span<const std::byte> bytes) {
		REQUIRE(bytes.size() <= world::PresentationStream::PACKET_BYTES);
		REQUIRE(receiving.Receive(bytes) == world::PresentationStreamReceive::Accepted);
	});
	ecs::Store replica("presentation stream replica");
	double now = 0;
	auto tick = [&] {
		now += 1.0 / 60.0;
		server.Poll(now);
		server.Advance(now);
		server.Flush(now);
		client.Poll(replica, now);
		client.Advance(now);
	};
	bool greeted = false;
	for (size_t attempt = 0; attempt < 256 && !ready; ++attempt) {
		tick();
		if (!greeted && client.Admitted()) greeted = client.SendUser(hello, now);
	}
	REQUIRE(ready);
	world::PresentationStreamFrame image;
	image.Message.From = {"far", "images", 9, 1};
	image.Message.To = {"near", "replies", 10, 1};
	image.Message.Sequence = 1;
	image.Message.Correlation = 72;
	image.Message.Payload.resize(128 * 128 * 8);
	for (size_t i = 0; i < image.Message.Payload.size(); ++i)
		image.Message.Payload[i] = static_cast<std::byte>(i % 251);
	REQUIRE(sending.Queue(image) == world::PresentationStatus::Ok);
	std::vector<world::PresentationStreamFrame> received;
	for (size_t attempt = 0; attempt < 1024 && received.empty(); ++attempt) {
		tick();
		if (losePacket && attempt == 0) clientWire.DropNext(1);
		sending.Flush([&](auto bytes) { return server.SendTo(peer, bytes, now); });
		received = receiving.Take();
	}
	REQUIRE(received.size() == 1);
	CHECK(received[0].Message.Payload == image.Message.Payload);
	CHECK(received[0].Message.From == image.Message.From);
	CHECK(received[0].Message.To == image.Message.To);
	CHECK(received[0].Message.Correlation == 72);
	CHECK(sending.Outgoing().Bytes == 0);
	CHECK(receiving.Incoming().Bytes == 0);
	CHECK(client.Live());
	CHECK(clientWire.Stats().Dropped == (losePacket ? 1 : 0));
}
