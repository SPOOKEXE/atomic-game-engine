#include <engine/assets/ChunkStore.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/assets/Signature.hpp>
#include <engine/delivery/GroupCodec.hpp>
#include <engine/delivery/Relay.hpp>
#include <engine/game/Content.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <server/ContentRelay.hpp>
#include <server/Server.hpp>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

TEST_SUITE_ID("server.contentrelay")
TEST_DEPENDS("engine.delivery.relay")
TEST_DEPENDS("engine.game.content")

using engine::assets::AssetKind;
using engine::assets::ChunkEntry;
using engine::assets::ChunkStore;
using engine::assets::ContentHash;
using engine::assets::Hasher;
using engine::assets::Manifest;
using engine::assets::SignatureBytes;
using engine::assets::SigningKey;
using engine::delivery::BundleRoute;
using engine::delivery::DeliverySettings;
using engine::delivery::MakeRouteFetcher;
using engine::delivery::Source;
using engine::delivery::SourceKind;
using engine::game::ContentChunk;
using engine::game::ContentRefusal;
using engine::game::ContentRouteRequest;
using engine::game::DecodeContentChunk;
using engine::game::DecodeContentRefusal;
using engine::game::EncodeContentRequest;
using engine::replication::ClientId;
using server::ContentMode;
using server::ContentModeOf;
using server::ContentRelay;
using server::ContentRelayLimits;
using server::Describe;
using server::Options;

namespace {
	namespace fs = std::filesystem;

	std::vector<std::byte> Bytes(std::string_view text) {
		std::vector<std::byte> data(text.size());
		std::memcpy(data.data(), text.data(), text.size());
		return data;
	}

	// A signed publication on disk, large enough that one route is several
	// chunks on the wire.
	struct Published {
		fs::path Root;
		Manifest Catalogue;
		ContentHash Bundle;
		std::vector<std::byte> AssetBytes;

		explicit Published(std::string_view name) {
			std::error_code failed;
			Root = fs::temp_directory_path() / ("atomic-serverrelay-" + std::string(name));
			fs::remove_all(Root, failed);

			std::optional<ChunkStore> store = ChunkStore::Open(Root, true);
			REQUIRE(store.has_value());

			// Deliberately incompressible-ish and well past one datagram, so the
			// chunking below is exercised rather than skipped.
			AssetBytes.reserve(40000);
			for (size_t index = 0; index < 40000; ++index) {
				AssetBytes.push_back(static_cast<std::byte>((index * 2654435761u) >> 13));
			}

			REQUIRE(store->Write(Hasher::Of(AssetBytes), AssetBytes));
			const std::vector<ChunkEntry> chunks{
				ChunkEntry{
					.Hash = Hasher::Of(AssetBytes),
					.Bytes = static_cast<uint32_t>(AssetBytes.size()),
				},
			};
			const ContentHash root = Catalogue.AddAsset("art/big.atex", AssetKind::Texture, chunks);
			const std::optional<ContentHash> bundle = Catalogue.AddBundle(std::span(&root, 1));
			REQUIRE(bundle.has_value());
			Bundle = *bundle;

			const std::vector<std::byte> seed(32, std::byte{0x33});
			const std::optional<SigningKey> key = SigningKey::FromSeed(seed);
			REQUIRE(key.has_value());
			const SignatureBytes signature = key->SignManifestRoot(Catalogue.Root());
			REQUIRE(store->WriteManifest(Catalogue, signature));
		}

		~Published() {
			std::error_code failed;
			fs::remove_all(Root, failed);
		}
	};

	std::unique_ptr<ContentRelay> RelayOver(const Published &store, const ContentRelayLimits &limits = {}) {
		DeliverySettings settings;
		settings.Sources.push_back(
			Source{
				.Name = "store",
				.Kind = SourceKind::Directory,
				.Location = store.Root.string(),
				.Enabled = true,
			}
		);
		return std::make_unique<ContentRelay>(MakeRouteFetcher(settings), limits);
	}

	// Whatever the relay handed to the link, in order.
	struct Wire {
		std::vector<std::vector<std::byte>> Sent;
		bool Refusing = false;

		std::function<bool(ClientId, std::span<const std::byte>)> Sender() {
			return [this](ClientId, std::span<const std::byte> payload) {
				if (Refusing) {
					return false;
				}
				Sent.emplace_back(payload.begin(), payload.end());
				return true;
			};
		}

		// Reassembles one ticket's route out of what was sent.
		std::vector<std::byte> Assemble(uint64_t ticket) const {
			std::vector<std::byte> whole;
			uint32_t filled = 0;
			for (const std::vector<std::byte> &message : Sent) {
				ContentChunk piece;
				if (!DecodeContentChunk(message, piece) || piece.Ticket != ticket) {
					continue;
				}
				if (whole.empty()) {
					whole.resize(piece.TotalBytes);
				}
				REQUIRE(piece.Offset == filled);
				std::copy(piece.Bytes.begin(), piece.Bytes.end(), whole.begin() + piece.Offset);
				filled += static_cast<uint32_t>(piece.Bytes.size());
			}
			whole.resize(filled);
			return whole;
		}

		bool Refused(uint64_t ticket) const {
			for (const std::vector<std::byte> &message : Sent) {
				ContentRefusal refusal;
				if (DecodeContentRefusal(message, refusal) && refusal.Ticket == ticket) {
					return true;
				}
			}
			return false;
		}
	};

	std::vector<std::byte> Ask(uint64_t ticket, std::string_view route) {
		return EncodeContentRequest(ContentRouteRequest{.Ticket = ticket, .Route = std::string(route)});
	}

	constexpr ClientId SOMEBODY{.Index = 3, .Generation = 1};
}

TEST_CASE("the content mode defaults to relay and is settings-only", "[server][content]") {
	// **The default is what the roadmap says mode 1 is**, and the *only* thing
	// that moves between the two is this field: there is no second server class
	// and no second settings type, so changing deployments is a configuration
	// change rather than a rebuild.
	const Options defaults;
	CHECK(defaults.ContentDelivery == ContentMode::Relay);
	CHECK(std::string_view(Describe(ContentMode::Relay)) == "relay");
	CHECK(std::string_view(Describe(ContentMode::Redirect)) == "redirect");

	REQUIRE(ContentModeOf("relay").has_value());
	CHECK(*ContentModeOf("relay") == ContentMode::Relay);
	REQUIRE(ContentModeOf("redirect").has_value());
	CHECK(*ContentModeOf("redirect") == ContentMode::Redirect);

	// **Named rather than defaulted.** A misspelling that silently meant relay
	// is a deployment that believes it redirected.
	CHECK_FALSE(ContentModeOf("Relay").has_value());
	CHECK_FALSE(ContentModeOf("proxy").has_value());
	CHECK_FALSE(ContentModeOf("").has_value());
}

TEST_CASE("a relayed route reaches the client in pieces", "[server][content]") {
	const Published store("stream");
	std::unique_ptr<ContentRelay> relay = RelayOver(store);
	Wire wire;

	REQUIRE(relay->Receive(SOMEBODY, Ask(1, BundleRoute(store.Bundle)), 0.0));
	CHECK(relay->Stats().Requests == 1);

	for (int pump = 0; pump < 64 && relay->Busy() != 0; ++pump) {
		relay->Pump(wire.Sender());
	}

	CHECK(relay->Stats().Served == 1);
	CHECK(relay->Busy() == 0);

	// More than one message, which is the whole reason a route is chunked: a
	// group is megabytes and a sealed datagram is about a kilobyte.
	CHECK(wire.Sent.size() > 1);

	const std::vector<std::byte> frame = wire.Assemble(1);
	const std::optional<std::vector<std::byte>> expanded =
		engine::delivery::GroupCodec::Decompress(frame, store.AssetBytes.size());
	REQUIRE(expanded.has_value());
	CHECK(*expanded == store.AssetBytes);
	CHECK(relay->Stats().SentBytes == frame.size());
}

TEST_CASE("a route this relay does not carry is refused", "[server][content]") {
	const Published store("refused");
	std::unique_ptr<ContentRelay> relay = RelayOver(store);
	Wire wire;

	// A route outside the closed list, and a bundle no publication describes.
	// Both are refusals and neither says why - a reason handed back over a wire
	// is an oracle, and the operator's log is where the detail belongs.
	REQUIRE(relay->Receive(SOMEBODY, Ask(1, "/ingest/mine"), 0.0));
	REQUIRE(relay->Receive(SOMEBODY, Ask(2, BundleRoute(Hasher::Of(Bytes("nothing")))), 0.0));

	for (int pump = 0; pump < 32 && relay->Busy() != 0; ++pump) {
		relay->Pump(wire.Sender());
	}

	CHECK(wire.Refused(1));
	CHECK(wire.Refused(2));
	CHECK(relay->Stats().Refused >= 2);
	CHECK(relay->Stats().Served == 0);
}

TEST_CASE("a client that spams retries is rate limited by the server", "[server][content]") {
	const Published store("flood");

	ContentRelayLimits limits;
	limits.RequestsPerSecond = 4.0;
	limits.Burst = 4.0;
	limits.OutstandingPerClient = 64;
	limits.FloodThreshold = 8;
	limits.FloodCooldownSeconds = 10.0;

	std::unique_ptr<ContentRelay> relay = RelayOver(store, limits);

	// **The bucket, at one instant.** Time is passed in, so a suite states the
	// rate rather than sleeping for it - `net`'s standing rule.
	for (uint64_t ticket = 1; ticket <= 4; ++ticket) {
		REQUIRE(relay->Receive(SOMEBODY, Ask(ticket, "/manifest"), 0.0));
	}
	CHECK(relay->Stats().Requests == 4);
	CHECK(relay->Stats().Dropped == 0);

	// The fifth in the same instant has nothing left to spend.
	REQUIRE(relay->Receive(SOMEBODY, Ask(5, "/manifest"), 0.0));
	CHECK(relay->Stats().Requests == 4);
	CHECK(relay->Stats().Dropped == 1);

	// A second later the bucket has refilled by exactly the rate.
	REQUIRE(relay->Receive(SOMEBODY, Ask(6, "/manifest"), 1.0));
	CHECK(relay->Stats().Requests == 5);

	SECTION("and a client that keeps at it is flagged and refused outright") {
		for (uint64_t ticket = 100; ticket < 140; ++ticket) {
			relay->Receive(SOMEBODY, Ask(ticket, "/manifest"), 1.0);
		}
		CHECK(relay->Stats().Flagged == 1);

		// **Refused rather than disconnected**, so a client on a bad script is
		// still distinguishable from one on a bad network - and it recovers.
		const uint64_t droppedWhileFlagged = relay->Stats().Dropped;
		relay->Receive(SOMEBODY, Ask(200, "/manifest"), 5.0);
		CHECK(relay->Stats().Dropped == droppedWhileFlagged + 1);

		relay->Receive(SOMEBODY, Ask(201, "/manifest"), 100.0);
		CHECK(relay->Stats().Dropped == droppedWhileFlagged + 1);
	}
}

TEST_CASE("one client's allowance is not another's", "[server][content]") {
	const Published store("perclient");

	ContentRelayLimits limits;
	limits.RequestsPerSecond = 1.0;
	limits.Burst = 1.0;
	std::unique_ptr<ContentRelay> relay = RelayOver(store, limits);

	constexpr ClientId first{.Index = 1, .Generation = 1};
	constexpr ClientId second{.Index = 2, .Generation = 1};

	relay->Receive(first, Ask(1, "/manifest"), 0.0);
	relay->Receive(first, Ask(2, "/manifest"), 0.0);
	relay->Receive(second, Ask(1, "/manifest"), 0.0);

	CHECK(relay->Stats().Requests == 2);
	CHECK(relay->Stats().Dropped == 1);
}

TEST_CASE("a slot reused by a new client starts with a fresh allowance", "[server][content]") {
	const Published store("generation");

	ContentRelayLimits limits;
	limits.RequestsPerSecond = 1.0;
	limits.Burst = 1.0;
	std::unique_ptr<ContentRelay> relay = RelayOver(store, limits);

	constexpr ClientId leaving{.Index = 5, .Generation = 1};
	constexpr ClientId arriving{.Index = 5, .Generation = 2};

	relay->Receive(leaving, Ask(1, "/manifest"), 0.0);
	relay->Receive(leaving, Ask(2, "/manifest"), 0.0);
	CHECK(relay->Stats().Dropped == 1);

	// **A slot is reused the moment somebody leaves**, and a bucket left behind
	// would hand the next client the previous one's punishment.
	relay->Receive(arriving, Ask(1, "/manifest"), 0.0);
	CHECK(relay->Stats().Requests == 2);
	CHECK(relay->Stats().Dropped == 1);
}

TEST_CASE("a client may not hold more routes than the bound", "[server][content]") {
	const Published store("outstanding");

	ContentRelayLimits limits;
	limits.OutstandingPerClient = 1;
	std::unique_ptr<ContentRelay> relay = RelayOver(store, limits);
	Wire wire;

	relay->Receive(SOMEBODY, Ask(1, "/manifest"), 0.0);
	relay->Receive(SOMEBODY, Ask(2, BundleRoute(store.Bundle)), 0.0);

	// The second is refused rather than queued: each outstanding route is a
	// group held in this process on that client's behalf, which is the resource
	// a flood is really after.
	CHECK(relay->Stats().Requests == 1);
	CHECK(relay->Stats().Refused == 1);

	for (int pump = 0; pump < 32 && relay->Busy() != 0; ++pump) {
		relay->Pump(wire.Sender());
	}
	CHECK(wire.Refused(2));
}

TEST_CASE("a link that will not take a chunk defers it rather than losing it", "[server][content]") {
	const Published store("deferred");
	std::unique_ptr<ContentRelay> relay = RelayOver(store);
	Wire wire;

	REQUIRE(relay->Receive(SOMEBODY, Ask(1, BundleRoute(store.Bundle)), 0.0));

	// **A tick whose budget the world already spent.** `Listener::SendTo`
	// refusing is ordinary backpressure, so nothing is dropped and nothing is
	// finished.
	wire.Refusing = true;
	for (int pump = 0; pump < 8; ++pump) {
		relay->Pump(wire.Sender());
	}
	CHECK(wire.Sent.empty());
	CHECK(relay->Stats().Deferred > 0);
	CHECK(relay->Stats().Served == 0);
	CHECK(relay->Busy() == 1);

	// And it resumes from exactly where it stopped.
	wire.Refusing = false;
	for (int pump = 0; pump < 64 && relay->Busy() != 0; ++pump) {
		relay->Pump(wire.Sender());
	}
	CHECK(relay->Stats().Served == 1);

	const std::vector<std::byte> frame = wire.Assemble(1);
	const std::optional<std::vector<std::byte>> expanded =
		engine::delivery::GroupCodec::Decompress(frame, store.AssetBytes.size());
	REQUIRE(expanded.has_value());
	CHECK(*expanded == store.AssetBytes);
}

TEST_CASE("a payload that is not a content message is somebody else's", "[server][content]") {
	const Published store("foreign");
	std::unique_ptr<ContentRelay> relay = RelayOver(store);

	// The user channel is shared, so the relay has to say "not mine" rather than
	// count a move as a malformed request.
	CHECK_FALSE(relay->Receive(SOMEBODY, Bytes("not a content message at all"), 0.0));
	CHECK(relay->Stats().Requests == 0);
	CHECK(relay->Stats().Dropped == 0);
}

TEST_CASE("a client that leaves takes its outstanding routes with it", "[server][content]") {
	const Published store("forget");
	std::unique_ptr<ContentRelay> relay = RelayOver(store);
	Wire wire;

	REQUIRE(relay->Receive(SOMEBODY, Ask(1, BundleRoute(store.Bundle)), 0.0));
	CHECK(relay->Busy() == 1);

	relay->Forget(SOMEBODY);
	CHECK(relay->Busy() == 0);

	for (int pump = 0; pump < 8; ++pump) {
		relay->Pump(wire.Sender());
	}
	CHECK(wire.Sent.empty());
}
