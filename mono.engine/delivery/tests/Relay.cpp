#include <engine/assets/ChunkStore.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/assets/Signature.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/delivery/Cache.hpp>
#include <engine/delivery/Client.hpp>
#include <engine/delivery/GroupCodec.hpp>
#include <engine/delivery/Relay.hpp>
#include <engine/net/http/Server.hpp>
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
using engine::delivery::RequestDiagnostics;
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
		std::string SecondAssetName = "art/wood.atex";
		std::vector<std::byte> SecondAssetBytes;

		explicit Published(std::string_view name, std::string_view seedByte = "5a", bool twoAssets = false) {
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
			std::vector<ContentHash> roots{root};
			if (twoAssets) {
				SecondAssetBytes = Bytes("a different texture for the second bundle member");
				const ContentHash chunk = Hasher::Of(SecondAssetBytes);
				REQUIRE(store->Write(chunk, SecondAssetBytes));
				roots.push_back(Catalogue.AddAsset(
					SecondAssetName,
					AssetKind::Texture,
					{ChunkEntry{.Hash = chunk, .Bytes = static_cast<uint32_t>(SecondAssetBytes.size())}}
				));
			}
			const std::optional<ContentHash> bundle = Catalogue.AddBundle(roots);
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
			if (route.starts_with("/bundle/") && (FailBundles || !CorruptBundle.empty())) {
				Finished.push_back(
					RelayAnswer{.Ticket = ticket, .Served = !FailBundles, .Bytes = CorruptBundle}
				);
				return true;
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
			if (Holding) {
				return;
			}
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
		bool Holding = false;
		bool FailBundles = false;
		std::vector<std::byte> CorruptBundle;

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
	CHECK(fetching->Diagnostics().Pending == 1);
	CHECK(fetching->Diagnostics().Ready == 0);
	CHECK(fetching->Diagnostics().Failed == 0);

	// Bounded rather than "until it works": a loop with no ceiling turns a
	// broken relay into a hung suite.
	for (int pump = 0; pump < 64 && fetching->StateOf(asked) == RequestState::Pending; ++pump) {
		fetching->Pump();
	}

	REQUIRE(fetching->StateOf(asked) == RequestState::Ready);
	CHECK(fetching->Diagnostics().Pending == 0);
	CHECK(fetching->Diagnostics().Ready == 1);
	const std::optional<engine::delivery::Asset> asset = fetching->Take(asked);
	REQUIRE(asset.has_value());
	CHECK(fetching->Diagnostics().Ready == 0);

	// **Verified against the signed manifest, over a relay.** The bytes came
	// through a hop that could have altered them and the check is the same one a
	// socket's bytes get, which is the property that makes relaying safe at all.
	CHECK(asset->Bytes == store.AssetBytes);
	CHECK(asset->Name == store.AssetName);
	CHECK(fetching->Counters().VerificationFailures == 0);
	CHECK(link.Asked >= 2);
}

TEST_CASE("failed relay requests release their fetch slots", "[delivery][relay]") {
	const Published store("failed-fetch-slots");
	DeliverySettings serving;
	serving.Publisher = store.Publisher;
	serving.Sources.push_back(store.Directory());
	LoopbackRelay link(MakeRouteFetcher(serving));
	link.FailBundles = true;
	auto client = MakeAssetClient(Relayed(store.Publisher), &link);
	REQUIRE(client != nullptr);
	for (int attempt = 0; attempt < 6; ++attempt) {
		const auto request = client->Request(store.AssetName);
		for (int pump = 0; pump < 64 && client->StateOf(request) == RequestState::Pending; ++pump) {
			client->Pump();
		}
		REQUIRE(client->StateOf(request) == RequestState::Failed);
		client->Cancel(request);
	}
}

TEST_CASE("relay releases failed upstream HTTP fetches before trying later requests", "[delivery][relay]") {
	const Published store("upstream-failed-slots");
	auto stalled = engine::net::http::Listen(0);
	REQUIRE(stalled != nullptr);
	DeliverySettings settings;
	settings.Publisher = store.Publisher;
	settings.Sources.push_back(
		Source{
			.Name = "stalled",
			.Kind = SourceKind::Http,
			.Location = "127.0.0.1:" + std::to_string(stalled->Local().Port),
			.Enabled = true,
		}
	);
	settings.Sources.push_back(store.Directory());
	engine::delivery::RouteFetcherSettings limits;
	limits.MaximumOutstanding = 1;
	limits.IdlePolls = 8;
	auto fetcher = MakeRouteFetcher(settings, limits);
	REQUIRE(fetcher != nullptr);
	for (int attempt = 0; attempt < 5; ++attempt) {
		const uint64_t ticket = fetcher->Request("/manifest");
		REQUIRE(ticket != 0);
		for (int pump = 0; pump < 500 && fetcher->StateOf(ticket) == RouteState::Pending; ++pump) {
			fetcher->Pump();
		}
		REQUIRE(fetcher->StateOf(ticket) == RouteState::Ready);
		std::vector<std::byte> result;
		REQUIRE(fetcher->Take(ticket, result));
		CHECK(result == WholeFile(store.Root / "manifest.acm"));
	}
}

TEST_CASE("corrupt relay bundle retries another source or fails", "[delivery][relay]") {
	const Published store("corrupt-bundle-fallback");
	DeliverySettings serving;
	serving.Publisher = store.Publisher;
	serving.Sources.push_back(store.Directory());
	LoopbackRelay link(MakeRouteFetcher(serving));
	auto wrong = store.AssetBytes;
	wrong[0] ^= std::byte{1};
	const auto compressed = GroupCodec::Compress(wrong);
	REQUIRE(compressed.has_value());
	link.CorruptBundle = *compressed;
	DeliverySettings settings = Relayed(store.Publisher);
	bool fallback = false;
	SECTION("good fallback") {
		settings.Sources.push_back(store.Directory());
		fallback = true;
	}
	SECTION("no fallback") {}
	auto client = MakeAssetClient(settings, &link);
	REQUIRE(client != nullptr);
	const auto request = client->Request(store.AssetName);
	for (int pump = 0; pump < 64 && client->StateOf(request) == RequestState::Pending; ++pump) {
		client->Pump();
	}
	if (fallback) {
		REQUIRE(client->StateOf(request) == RequestState::Ready);
		const auto asset = client->Take(request);
		REQUIRE(asset.has_value());
		CHECK(asset->Bytes == store.AssetBytes);
	} else {
		CHECK(client->StateOf(request) == RequestState::Failed);
	}
	CHECK(client->Counters().VerificationFailures == 1);
}

TEST_CASE("corrupt later bundle member publishes no earlier members", "[delivery][relay]") {
	const Published store("corrupt-later-member", "5a", true);
	const auto *bundle = store.Catalogue.FindBundle(store.Bundle);
	REQUIRE(bundle != nullptr);
	REQUIRE(bundle->Assets.size() == 2);
	const auto *first = store.Catalogue.FindByRoot(bundle->Assets[0]);
	const auto *second = store.Catalogue.FindByRoot(bundle->Assets[1]);
	REQUIRE(first != nullptr);
	REQUIRE(second != nullptr);
	const auto slice = store.Catalogue.SliceOf(*bundle, second->Root);
	REQUIRE(slice.has_value());
	REQUIRE(slice->Bytes > 0);
	auto source = ChunkStore::Open(store.Root, false);
	REQUIRE(source.has_value());
	auto payload = source->ReadBundle(store.Catalogue, *bundle);
	REQUIRE(payload.has_value());
	(*payload)[slice->Offset] ^= std::byte{1};
	auto compressed = GroupCodec::Compress(*payload);
	REQUIRE(compressed.has_value());

	DeliverySettings serving;
	serving.Publisher = store.Publisher;
	serving.Sources.push_back(store.Directory());
	LoopbackRelay link(MakeRouteFetcher(serving));
	link.CorruptBundle = *compressed;
	DeliverySettings settings = Relayed(store.Publisher);
	settings.CachePath = store.Root / "cache";
	bool fallback = false;
	SECTION("good fallback") {
		settings.Sources.push_back(store.Directory());
		fallback = true;
	}
	SECTION("no fallback") {}
	auto client = MakeAssetClient(settings, &link);
	REQUIRE(client != nullptr);
	const auto firstRequest = client->Request(first->Name);
	const auto secondRequest = client->Request(second->Name);
	for (int pump = 0; pump < 64 && client->Counters().VerificationFailures == 0; ++pump) {
		client->Pump();
	}
	REQUIRE(client->Counters().VerificationFailures == 1);
	CHECK(client->StateOf(firstRequest) != RequestState::Ready);
	CHECK(client->StateOf(secondRequest) != RequestState::Ready);
	auto cache = engine::delivery::ContentCache::Open(settings.CachePath, settings.CacheCapacityBytes);
	REQUIRE(cache.has_value());
	CHECK_FALSE(cache->Contains(first->Root));
	CHECK_FALSE(cache->Contains(second->Root));
	for (int pump = 0; pump < 64 && client->StateOf(firstRequest) == RequestState::Pending; ++pump) {
		client->Pump();
	}
	if (fallback) {
		REQUIRE(client->StateOf(firstRequest) == RequestState::Ready);
		REQUIRE(client->StateOf(secondRequest) == RequestState::Ready);
		const auto firstAsset = client->Take(firstRequest);
		const auto secondAsset = client->Take(secondRequest);
		REQUIRE(firstAsset.has_value());
		REQUIRE(secondAsset.has_value());
		CHECK(engine::assets::VerifyAsset(*first, firstAsset->Bytes));
		CHECK(engine::assets::VerifyAsset(*second, secondAsset->Bytes));
	} else {
		CHECK(client->StateOf(firstRequest) == RequestState::Failed);
		CHECK(client->StateOf(secondRequest) == RequestState::Failed);
	}
}

TEST_CASE("request diagnostics separate settled records from active delivery", "[delivery][direct][relay]") {
	const Published store("diagnostics");
	DeliverySettings direct;
	direct.Publisher = store.Publisher;
	direct.Sources.push_back(store.Directory());
	std::unique_ptr<engine::delivery::AssetClient> fetching = MakeAssetClient(direct);
	REQUIRE(fetching != nullptr);

	const engine::delivery::RequestId ready = fetching->Request(store.AssetName);
	const engine::delivery::RequestId failed = fetching->Request("missing.asset");
	const engine::delivery::RequestId cancelled = fetching->Request("cancelled.asset");
	CHECK(fetching->Cancel(cancelled));
	for (int pump = 0; pump < 16 && fetching->Diagnostics().Pending > 0; ++pump) {
		fetching->Pump();
	}

	const RequestDiagnostics settled = fetching->Diagnostics();
	CHECK(fetching->StateOf(ready) == RequestState::Ready);
	CHECK(fetching->StateOf(failed) == RequestState::Failed);
	CHECK(settled.Pending == 0);
	CHECK(settled.Ready == 1);
	CHECK(settled.Failed == 1);
	CHECK(settled.TransportActive == 0);
	CHECK(fetching->Outstanding() == 2);
	REQUIRE(fetching->Take(ready).has_value());
	CHECK(fetching->Diagnostics().Ready == 0);
	CHECK(fetching->Diagnostics().Failed == 1);
}

TEST_CASE("completed directory bundle is not read again on the next pump", "[delivery][direct]") {
	const Published store("directory-completed-once");
	DeliverySettings settings;
	settings.Publisher = store.Publisher;
	settings.Sources.push_back(store.Directory());
	auto client = MakeAssetClient(settings);
	REQUIRE(client != nullptr);

	const auto first = client->Request(store.AssetName);
	const auto second = client->Request(store.AssetName);
	for (int pump = 0; pump < 16 && client->StateOf(first) == RequestState::Pending; ++pump) {
		client->Pump();
	}
	REQUIRE(client->StateOf(first) == RequestState::Ready);
	REQUIRE(client->StateOf(second) == RequestState::Ready);
	CHECK(client->Counters().Bundles == 1);
	client->Pump();
	CHECK(client->Counters().Bundles == 1);
	CHECK(client->Counters().TransferredBytes == store.AssetBytes.size());
	const auto firstAsset = client->Take(first);
	const auto secondAsset = client->Take(second);
	REQUIRE(firstAsset.has_value());
	REQUIRE(secondAsset.has_value());
	CHECK(firstAsset->Bytes == store.AssetBytes);
	CHECK(secondAsset->Bytes == store.AssetBytes);

	// With no cache, a later request is a real new demand and reads the bundle again.
	const auto later = client->Request(store.AssetName);
	for (int pump = 0; pump < 16 && client->StateOf(later) == RequestState::Pending; ++pump) {
		client->Pump();
	}
	REQUIRE(client->StateOf(later) == RequestState::Ready);
	const auto immediate = client->Request(store.AssetName);
	client->Pump();
	REQUIRE(client->StateOf(immediate) == RequestState::Ready);
	client->Pump();
	CHECK(client->Counters().Bundles == 3);
	const auto laterAsset = client->Take(later);
	const auto immediateAsset = client->Take(immediate);
	REQUIRE(laterAsset.has_value());
	REQUIRE(immediateAsset.has_value());
	CHECK(laterAsset->Bytes == store.AssetBytes);
	CHECK(immediateAsset->Bytes == store.AssetBytes);
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

TEST_CASE("relay diagnostics report only held transport as active", "[delivery][relay]") {
	const Published store("diagnostic-active");
	DeliverySettings serving;
	serving.Publisher = store.Publisher;
	serving.Sources.push_back(store.Directory());
	LoopbackRelay link(MakeRouteFetcher(serving));
	link.Holding = true;
	std::unique_ptr<engine::delivery::AssetClient> fetching =
		MakeAssetClient(Relayed(store.Publisher), &link);
	REQUIRE(fetching != nullptr);
	const engine::delivery::RequestId asked = fetching->Request(store.AssetName);
	fetching->Pump();
	CHECK(fetching->StateOf(asked) == RequestState::Pending);
	CHECK(fetching->Diagnostics().TransportActive == 1);

	link.Holding = false;
	for (int pump = 0; pump < 64 && fetching->StateOf(asked) == RequestState::Pending; ++pump)
		fetching->Pump();
	REQUIRE(fetching->StateOf(asked) == RequestState::Ready);
	CHECK(fetching->Diagnostics().TransportActive == 0);
	CHECK(fetching->Diagnostics().Ready == 1);
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
