#include <engine/assets/ChunkStore.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/assets/Signature.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/delivery/Client.hpp>
#include <engine/delivery/GroupCodec.hpp>
#include <engine/delivery/Relay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

TEST_SUITE_ID("engine.delivery.relay")
TEST_DEPENDS("engine.assets.chunkstore")
TEST_DEPENDS("engine.assets.manifest")
TEST_DEPENDS("engine.delivery.groupcodec")
TEST_DEPENDS("engine.delivery.source")

using engine::assets::AssetKind;
using engine::assets::ChunkEntry;
using engine::assets::ChunkStore;
using engine::assets::ContentHash;
using engine::assets::Hasher;
using engine::assets::Manifest;
using engine::assets::PublicKey;
using engine::assets::SignatureBytes;
using engine::assets::SigningKey;
using engine::delivery::BundleOfRoute;
using engine::delivery::BundleRoute;
using engine::delivery::DeliverySettings;
using engine::delivery::GroupCodec;
using engine::delivery::MakeAssetClient;
using engine::delivery::MakeRelayClient;
using engine::delivery::MakeRouteFetcher;
using engine::delivery::RelayableRoute;
using engine::delivery::RelayAnswer;
using engine::delivery::RelayChannel;
using engine::delivery::RequestState;
using engine::delivery::RouteFetcher;
using engine::delivery::RouteState;
using engine::delivery::Source;
using engine::delivery::SourceKind;

namespace {
	namespace fs = std::filesystem;

	std::vector<std::byte> Bytes(std::string_view text) {
		std::vector<std::byte> data(text.size());
		std::memcpy(data.data(), text.data(), text.size());
		return data;
	}

	// A directory holding a signed, single-bundle publication.
	//
	// **Built by hand rather than through `cdn::Publisher`**, because that is a
	// program's library and this is an engine module's suite - the tier rule
	// says which way that edge may point. What it produces is the same tree:
	// chunks named by their own hashes, `manifest.acm` as a signature followed
	// by the manifest, and nothing else.
	struct Published {
		fs::path Root;
		Manifest Catalogue;
		PublicKey Publisher;
		ContentHash Bundle;
		std::string AssetName = "art/stone.atex";
		std::vector<std::byte> AssetBytes;

		explicit Published(std::string_view name, std::string_view seedByte = "5a") {
			std::error_code failed;
			Root = fs::temp_directory_path() / ("atomic-relay-" + std::string(name));
			fs::remove_all(Root, failed);

			std::optional<ChunkStore> store = ChunkStore::Open(Root, true);
			REQUIRE(store.has_value());

			// Two chunks, so an asset root is a tree over more than one hash -
			// the shape where "hash the whole thing" stops looking right.
			const std::vector<std::byte> first = Bytes("the first half of a texture");
			const std::vector<std::byte> second = Bytes("and the second half of it");
			AssetBytes = first;
			AssetBytes.insert(AssetBytes.end(), second.begin(), second.end());

			REQUIRE(store->Write(Hasher::Of(first), first));
			REQUIRE(store->Write(Hasher::Of(second), second));

			const std::vector<ChunkEntry> chunks{
				ChunkEntry{.Hash = Hasher::Of(first), .Bytes = static_cast<uint32_t>(first.size())},
				ChunkEntry{.Hash = Hasher::Of(second), .Bytes = static_cast<uint32_t>(second.size())},
			};
			const ContentHash root = Catalogue.AddAsset(AssetName, AssetKind::Texture, chunks);
			const std::optional<ContentHash> bundle = Catalogue.AddBundle(std::span(&root, 1));
			REQUIRE(bundle.has_value());
			Bundle = *bundle;

			std::vector<std::byte> seed(32, std::byte{0});
			seed[0] = static_cast<std::byte>(std::stoi(std::string(seedByte), nullptr, 16));
			const std::optional<SigningKey> key = SigningKey::FromSeed(seed);
			REQUIRE(key.has_value());
			Publisher = key->Public();

			const SignatureBytes signature = key->SignManifestRoot(Catalogue.Root());
			REQUIRE(store->WriteManifest(Catalogue, signature));
		}

		~Published() {
			std::error_code failed;
			fs::remove_all(Root, failed);
		}

		Source Directory() const {
			return Source{
				.Name = "store",
				.Kind = SourceKind::Directory,
				.Location = Root.string(),
				.Enabled = true,
			};
		}
	};

	// Reads a whole file, for the "verbatim" check the manifest route makes.
	std::vector<std::byte> WholeFile(const fs::path &path) {
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		REQUIRE(file);
		const auto size = static_cast<size_t>(file.tellg());
		std::vector<std::byte> bytes(size);
		file.seekg(0);
		file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size));
		return bytes;
	}

	// A relay whose far end is a `RouteFetcher` in this process.
	//
	// **What relay mode is, with the wire taken out.** The server half asks its
	// own sources for a route and the client half hands what comes back to an
	// ordinary delivery client - so what this exercises is exactly the path a
	// game link carries, minus the framing that `game/tests/Content.cpp` and
	// `server/tests/ContentRelay.cpp` pin separately.
	class LoopbackRelay final : public RelayChannel {
	  public:
		explicit LoopbackRelay(std::unique_ptr<RouteFetcher> answering) : Answering(std::move(answering)) {}

		bool Ask(uint64_t ticket, std::string_view route) override {
			if (Refusing) {
				return false;
			}
			const uint64_t fetch = Answering->Request(route);
			if (fetch == 0) {
				Finished.push_back(RelayAnswer{.Ticket = ticket, .Served = false, .Bytes = {}});
				return true;
			}
			Live.emplace_back(ticket, fetch);
			++Asked;
			return true;
		}

		void Collect(std::vector<RelayAnswer> &into) override {
			Answering->Pump();
			for (auto entry = Live.begin(); entry != Live.end();) {
				const RouteState state = Answering->StateOf(entry->second);
				if (state == RouteState::Pending) {
					++entry;
					continue;
				}
				RelayAnswer answer;
				answer.Ticket = entry->first;
				answer.Served = state == RouteState::Ready;
				Answering->Take(entry->second, answer.Bytes);
				Finished.push_back(std::move(answer));
				entry = Live.erase(entry);
			}
			for (RelayAnswer &answer : Finished) {
				into.push_back(std::move(answer));
			}
			Finished.clear();
		}

		void Abandon(uint64_t ticket) override {
			for (auto entry = Live.begin(); entry != Live.end(); ++entry) {
				if (entry->first == ticket) {
					Answering->Cancel(entry->second);
					Live.erase(entry);
					return;
				}
			}
		}

		// Whether the link refuses to carry anything, which is what a full
		// per-tick budget looks like from here.
		bool Refusing = false;

		// How many routes actually reached the far end.
		size_t Asked = 0;

	  private:
		std::unique_ptr<RouteFetcher> Answering;
		std::vector<std::pair<uint64_t, uint64_t>> Live;
		std::vector<RelayAnswer> Finished;
	};

	DeliverySettings Relayed(const PublicKey &publisher) {
		DeliverySettings settings;
		settings.Publisher = publisher;
		settings.Sources.push_back(
			Source{
				.Name = "server",
				.Kind = SourceKind::Relay,
				.Location = "the game link",
				.Enabled = true,
			}
		);
		return settings;
	}
}

TEST_CASE("a bundle route round-trips its hash", "[delivery][relay]") {
	const ContentHash bundle = Hasher::Of(Bytes("a bundle"));
	const std::string route = BundleRoute(bundle);

	CHECK(route == "/bundle/" + bundle.ToHex());
	REQUIRE(BundleOfRoute(route).has_value());
	CHECK(*BundleOfRoute(route) == bundle);
}

TEST_CASE("only the three routes an origin serves may be relayed", "[delivery][relay]") {
	CHECK(RelayableRoute("/manifest"));
	CHECK(RelayableRoute("/dictionary"));
	CHECK(RelayableRoute(BundleRoute(Hasher::Of(Bytes("x")))));

	// **A closed list rather than a prefix test.** Every one of these is a route
	// something in this engine serves or might, and none of them is a route a
	// game link was ever meant to carry - an ingest hop through a relay would be
	// a client publishing through somebody else's credentials.
	CHECK_FALSE(RelayableRoute("/health"));
	CHECK_FALSE(RelayableRoute("/catalogue"));
	CHECK_FALSE(RelayableRoute("/ingest/thing"));
	CHECK_FALSE(RelayableRoute("/bundle/not-a-hash"));
	CHECK_FALSE(RelayableRoute("/bundle/"));
	CHECK_FALSE(RelayableRoute(""));
	CHECK_FALSE(RelayableRoute("/manifest/../ingest"));
}

TEST_CASE("a route fetcher serves a store's three routes", "[delivery][relay]") {
	const Published store("routes");

	DeliverySettings settings;
	settings.Publisher = store.Publisher;
	settings.Sources.push_back(store.Directory());

	std::unique_ptr<RouteFetcher> fetcher = MakeRouteFetcher(settings);
	REQUIRE(fetcher != nullptr);

	SECTION("the manifest is the published file, byte for byte") {
		const uint64_t ticket = fetcher->Request("/manifest");
		REQUIRE(ticket != 0);
		fetcher->Pump();
		REQUIRE(fetcher->StateOf(ticket) == RouteState::Ready);

		std::vector<std::byte> served;
		REQUIRE(fetcher->Take(ticket, served));

		// **Verbatim, not a re-serialisation.** What a publisher signed is a
		// file; a relay that rebuilt it would make a client verify something
		// nobody ever signed.
		CHECK(served == WholeFile(store.Root / ChunkStore::MANIFEST_FILE));
	}

	SECTION("a bundle comes back as a frame that expands to the group") {
		const uint64_t ticket = fetcher->Request(BundleRoute(store.Bundle));
		REQUIRE(ticket != 0);
		fetcher->Pump();
		REQUIRE(fetcher->StateOf(ticket) == RouteState::Ready);

		std::vector<std::byte> frame;
		REQUIRE(fetcher->Take(ticket, frame));

		// A store holds chunks and a relay carries a frame, so this end is
		// where the group was compressed.
		const std::optional<std::vector<std::byte>> expanded =
			GroupCodec::Decompress(frame, store.AssetBytes.size());
		REQUIRE(expanded.has_value());
		CHECK(*expanded == store.AssetBytes);
	}

	SECTION("a store with no dictionary refuses that route rather than serving nothing") {
		const uint64_t ticket = fetcher->Request("/dictionary");
		REQUIRE(ticket != 0);
		fetcher->Pump();
		CHECK(fetcher->StateOf(ticket) == RouteState::Refused);
		CHECK(fetcher->Counters().Refused == 1);
	}

	SECTION("a route this relay does not carry is never issued a ticket") {
		CHECK(fetcher->Request("/ingest/thing") == 0);
		CHECK(fetcher->Request("/health") == 0);
		CHECK(fetcher->Outstanding() == 0);
	}

	SECTION("a bundle no source has is refused after every source is tried") {
		const uint64_t ticket = fetcher->Request(BundleRoute(Hasher::Of(Bytes("nothing"))));
		REQUIRE(ticket != 0);
		fetcher->Pump();
		CHECK(fetcher->StateOf(ticket) == RouteState::Refused);
		CHECK(fetcher->Counters().SourceFailures == 1);
	}
}

TEST_CASE("a relay passes over a manifest that does not verify", "[delivery][relay]") {
	const Published store("wrongkey");
	const Published other("otherkey", "7c");

	DeliverySettings settings;
	// The publisher of a *different* publication, which is what a relay pointed
	// at somebody else's origin looks like.
	settings.Publisher = other.Publisher;
	settings.Sources.push_back(store.Directory());

	std::unique_ptr<RouteFetcher> fetcher = MakeRouteFetcher(settings);
	REQUIRE(fetcher != nullptr);

	const uint64_t ticket = fetcher->Request("/manifest");
	REQUIRE(ticket != 0);
	fetcher->Pump();

	CHECK(fetcher->StateOf(ticket) == RouteState::Refused);

	// **Counted apart from a source being down**, which is the whole point of
	// having two numbers: one is an operational event and the other is either
	// corruption or somebody serving content the publisher did not write.
	CHECK(fetcher->Counters().VerificationFailures == 1);
	CHECK(fetcher->Counters().SourceFailures == 1);
}

TEST_CASE("a route fetcher falls back after an unavailable higher-priority source", "[delivery][relay]") {
	const Published store("fallback");
	DeliverySettings settings;
	settings.Publisher = store.Publisher;
	settings.Sources.push_back(
		Source{
			.Name = "offline",
			.Kind = SourceKind::Directory,
			.Location = (store.Root / "missing").string(),
			.Enabled = true,
		}
	);
	settings.Sources.push_back(store.Directory());
	std::unique_ptr<RouteFetcher> fetcher = MakeRouteFetcher(settings);
	REQUIRE(fetcher != nullptr);

	const uint64_t ticket = fetcher->Request("/manifest");
	REQUIRE(ticket != 0);
	for (size_t attempt = 0; attempt < 8 && fetcher->StateOf(ticket) == RouteState::Pending; attempt++) {
		fetcher->Pump();
	}
	CHECK(fetcher->StateOf(ticket) == RouteState::Ready);
}

TEST_CASE("a relay with no publisher key forwards without checking", "[delivery][relay]") {
	const Published store("nokey");

	DeliverySettings settings;
	settings.Sources.push_back(store.Directory());

	std::unique_ptr<RouteFetcher> fetcher = MakeRouteFetcher(settings);
	REQUIRE(fetcher != nullptr);

	const uint64_t ticket = fetcher->Request("/manifest");
	REQUIRE(ticket != 0);
	fetcher->Pump();

	// The honest state of a relay whose operator was never given the
	// publisher's key. The client checks it end to end regardless, which is why
	// this is defence in depth rather than the boundary.
	CHECK(fetcher->StateOf(ticket) == RouteState::Ready);
	CHECK(fetcher->Counters().VerificationFailures == 0);
}

TEST_CASE("a client fetches and verifies content over a relay", "[delivery][relay]") {
	const Published store("endtoend");

	DeliverySettings serving;
	serving.Publisher = store.Publisher;
	serving.Sources.push_back(store.Directory());
	LoopbackRelay link(MakeRouteFetcher(serving));

	std::unique_ptr<engine::delivery::AssetClient> fetching =
		MakeAssetClient(Relayed(store.Publisher), &link);
	REQUIRE(fetching != nullptr);

	const engine::delivery::RequestId asked = fetching->Request(store.AssetName);

	// Bounded rather than "until it works": a loop with no ceiling turns a
	// broken relay into a hung suite.
	for (int pump = 0; pump < 64 && fetching->StateOf(asked) == RequestState::Pending; ++pump) {
		fetching->Pump();
	}

	REQUIRE(fetching->StateOf(asked) == RequestState::Ready);
	const std::optional<engine::delivery::Asset> asset = fetching->Take(asked);
	REQUIRE(asset.has_value());

	// **Verified against the signed manifest, over a relay.** The bytes came
	// through a hop that could have altered them and the check is the same one a
	// socket's bytes get, which is the property that makes relaying safe at all.
	CHECK(asset->Bytes == store.AssetBytes);
	CHECK(asset->Name == store.AssetName);
	CHECK(fetching->Counters().VerificationFailures == 0);
	CHECK(link.Asked >= 2);
}

TEST_CASE("a relay that will not carry a request is retried, not failed", "[delivery][relay]") {
	const Published store("backpressure");

	DeliverySettings serving;
	serving.Publisher = store.Publisher;
	serving.Sources.push_back(store.Directory());
	LoopbackRelay link(MakeRouteFetcher(serving));
	link.Refusing = true;

	std::unique_ptr<engine::delivery::AssetClient> fetching =
		MakeAssetClient(Relayed(store.Publisher), &link);
	REQUIRE(fetching != nullptr);

	const engine::delivery::RequestId asked = fetching->Request(store.AssetName);
	for (int pump = 0; pump < 8; ++pump) {
		fetching->Pump();
	}

	// A link that would not take the request is ordinary backpressure, exactly
	// as `Link::Reserve` refusing is, so nothing has failed yet.
	CHECK(fetching->StateOf(asked) == RequestState::Pending);
	CHECK(link.Asked == 0);

	link.Refusing = false;
	for (int pump = 0; pump < 64 && fetching->StateOf(asked) == RequestState::Pending; ++pump) {
		fetching->Pump();
	}
	CHECK(fetching->StateOf(asked) == RequestState::Ready);
}

TEST_CASE("a relay source with nothing to carry it is skipped", "[delivery][relay]") {
	const Published store("nolink");

	DeliverySettings settings = Relayed(store.Publisher);
	settings.Sources.insert(settings.Sources.begin(), store.Directory());

	// No channel. The relay entry is dropped with a warning and the directory
	// beside it still works, because the rest of the list is perfectly good.
	std::unique_ptr<engine::delivery::AssetClient> fetching = MakeAssetClient(settings, nullptr);
	REQUIRE(fetching != nullptr);

	const engine::delivery::RequestId asked = fetching->Request(store.AssetName);
	for (int pump = 0; pump < 16 && fetching->StateOf(asked) == RequestState::Pending; ++pump) {
		fetching->Pump();
	}
	CHECK(fetching->StateOf(asked) == RequestState::Ready);
}

TEST_CASE("a route fetcher refuses settings that name no usable source", "[delivery][relay]") {
	DeliverySettings settings;
	CHECK(MakeRouteFetcher(settings) == nullptr);

	// A relay source cannot answer for a relay: this end has no channel to ask
	// through, and pretending otherwise would refuse every route in silence.
	settings.Sources.push_back(
		Source{.Name = "onward", .Kind = SourceKind::Relay, .Location = "somewhere", .Enabled = true}
	);
	std::unique_ptr<RouteFetcher> fetcher = MakeRouteFetcher(settings);
	REQUIRE(fetcher != nullptr);
	CHECK(fetcher->Request("/manifest") == 0);
}
