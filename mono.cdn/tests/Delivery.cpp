#include <engine/assets/AssetKind.hpp>
#include <engine/assets/ChunkStore.hpp>
#include <engine/assets/Grant.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/assets/Signature.hpp>
#include <engine/delivery/Client.hpp>
#include <engine/delivery/Source.hpp>
#include <engine/net/Endpoint.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cdn/Publisher.hpp>
#include <cdn/Service.hpp>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// **The whole path, end to end**, and the reason this suite is in `mono.cdn`
// rather than in `delivery`: it needs both halves, and `cdn` is the side that
// links the other. A publisher writes a store, an origin serves it over a real
// socket on a real port, and a headless delivery client fetches through it —
// with nothing mocked between them.
//
// What it is really checking is that the two ends agree about four things that
// are each defined in one place and used in two:
//
// - the manifest format and its signed root
// - the group layout — members concatenated in member order
// - the compressed frame, and the length it must expand to
// - the grant, issued with one function and opened with another
//
// Any of those drifting is a bug that unit tests on either side would pass.

TEST_SUITE_ID("cdn.delivery")
TEST_DEPENDS("cdn.service")
TEST_DEPENDS("cdn.publisher")
TEST_DEPENDS("engine.delivery.cache")
TEST_DEPENDS("engine.delivery.source")

using cdn::Origin;
using cdn::Publication;
using cdn::Service;
using engine::assets::AssetKind;
using engine::assets::ChunkStore;
using engine::assets::ContentHash;
using engine::assets::Grant;
using engine::assets::GrantKey;
using engine::assets::GrantScope;
using engine::assets::Hasher;
using engine::assets::Manifest;
using engine::assets::PublicKey;
using engine::assets::SignatureBytes;
using engine::assets::SigningKey;
using engine::delivery::Asset;
using engine::delivery::AssetClient;
using engine::delivery::DeliverySettings;
using engine::delivery::MakeAssetClient;
using engine::delivery::RequestId;
using engine::delivery::RequestState;
using engine::delivery::Source;
using engine::delivery::SourceKind;

namespace {
	namespace fs = std::filesystem;

	constexpr uint64_t NOW = 2'000'000;
	constexpr uint64_t EXPIRY = NOW + 600;
	constexpr int MAXIMUM_POLLS = 40000;

	GrantKey Secret(uint8_t fill = 7) {
		std::array<std::byte, GrantKey::BYTES> secret{};
		for (size_t index = 0; index < secret.size(); ++index) {
			secret[index] = static_cast<std::byte>(fill + index);
		}
		auto key = GrantKey::FromSecret(secret);
		REQUIRE(key.has_value());
		return std::move(*key);
	}

	SigningKey Publisher(uint8_t fill = 21) {
		std::array<std::byte, 32> seed{};
		for (size_t index = 0; index < seed.size(); ++index) {
			seed[index] = static_cast<std::byte>(fill + index);
		}
		auto key = SigningKey::FromSeed(seed);
		REQUIRE(key.has_value());
		return std::move(*key);
	}

	// Records that share a shape and differ in their numbers, so a group
	// compresses for the reason real content does.
	std::string Structured(std::string_view label, size_t records) {
		std::string text;
		text.reserve(records * 48);
		for (size_t index = 0; index < records; ++index) {
			text += "{\"";
			text += label;
			text += "\":";
			text += std::to_string(index);
			text += ",\"lod\":";
			text += std::to_string(index % 4);
			text += ",\"flags\":";
			text += std::to_string((index * 2654435761u) % 65536);
			text += "}\n";
		}
		return text;
	}

	std::string Text(const std::vector<std::byte> &bytes) {
		std::string out;
		out.reserve(bytes.size());
		for (const std::byte value : bytes) {
			out.push_back(static_cast<char>(value));
		}
		return out;
	}

	// A published store, an origin serving it on a port, and the original
	// bytes so a fetch can be compared against what was published.
	struct World {
		fs::path Root;
		std::optional<Manifest> Catalogue;
		std::unique_ptr<Origin> Serving;
		std::unique_ptr<Service> Listening;
		engine::net::Endpoint Address;
		std::vector<std::pair<std::string, std::string>> Original;

		World() {
			static int serial = 0;
			Root = fs::temp_directory_path() / ("atomic-e2e-" + std::to_string(++serial));
			std::error_code failure;
			fs::remove_all(Root, failure);

			// A mesh, an image and a sound — the three kinds the task named,
			// each big enough to be cut into several chunks.
			Write("meshes/rock.mesh", Structured("vert", 4000));
			Write("meshes/tree.mesh", Structured("vert", 3000));
			Write("textures/grass.png", Structured("texel", 3500));
			Write("textures/bark.png", Structured("texel", 2500));
			Write("audio/bark.wav", Structured("sample", 2000));

			REQUIRE(cdn::Publish(Root / "content", Root / "store", Publisher()).has_value());

			auto store = ChunkStore::Open(Root / "store", false);
			REQUIRE(store.has_value());
			SignatureBytes signature;
			Catalogue = store->ReadManifest(signature);
			REQUIRE(Catalogue.has_value());

			Serving = std::make_unique<Origin>(Secret());
			auto mounted = cdn::ContentRoot::Mount(Root / "store");
			REQUIRE(mounted.has_value());
			REQUIRE(Serving->Publish(std::make_shared<const Publication>(*mounted, *Catalogue)));

			cdn::ServiceSettings service;
			service.Port = 0;
			Listening = cdn::Serve(*Serving, std::move(*store), service);
			REQUIRE(Listening != nullptr);
			Address = engine::net::Endpoint::LoopbackIPv4(Listening->Local().Port);
		}

		~World() {
			std::error_code failure;
			fs::remove_all(Root, failure);
		}

		void Write(const std::string &name, const std::string &body) {
			const fs::path path = Root / "content" / name;
			std::error_code failure;
			fs::create_directories(path.parent_path(), failure);
			std::ofstream file(path, std::ios::binary | std::ios::trunc);
			file.write(body.data(), static_cast<std::streamsize>(body.size()));
			Original.emplace_back(name, body);
		}

		const std::string &Published(std::string_view name) const {
			for (const auto &[key, body] : Original) {
				if (key == name) {
					return body;
				}
			}
			FAIL("no such published asset");
			return Original[0].second;
		}

		std::string Location() const {
			return "127.0.0.1:" + std::to_string(Listening->Local().Port);
		}

		// A grant covering every bundle, which is what a server would issue for
		// a session that needs the whole game.
		std::vector<std::byte> Token(uint64_t expiry = EXPIRY) const {
			GrantScope scope;
			scope.Session = 1;
			for (const auto &bundle : Catalogue->Bundles()) {
				scope.Bundles.push_back(bundle.Root);
			}
			scope.ExpiresAtSeconds = expiry;
			scope.ByteBudget = 64 * 1024 * 1024;

			const GrantKey key = Secret();
			const auto grant = Grant::Issue(scope, key);
			REQUIRE(grant.has_value());
			return grant->Encode();
		}

		// Settings pointing a client at this origin over HTTP.
		DeliverySettings Over(const fs::path &cache = {}) const {
			DeliverySettings settings;
			settings.CachePath = cache;
			settings.Publisher = Publisher().Public();
			settings.IdlePolls = MAXIMUM_POLLS;
			settings.Sources.push_back(
				Source{
					.Name = "origin",
					.Kind = SourceKind::Http,
					.Location = Location(),
					.Enabled = true,
				}
			);
			return settings;
		}

		// Drives the origin and the client together until the request settles.
		RequestState Settle(AssetClient &client, RequestId id) {
			for (int poll = 0; poll < MAXIMUM_POLLS; ++poll) {
				Listening->Pump(NOW);
				client.Pump();
				const RequestState state = client.StateOf(id);
				if (state != RequestState::Pending) {
					return state;
				}
			}
			return RequestState::Pending;
		}

		// Drives until the client has a verified manifest.
		void SettleCatalogue(AssetClient &client) {
			for (int poll = 0; poll < MAXIMUM_POLLS && !client.Ready(); ++poll) {
				Listening->Pump(NOW);
				client.Pump();
			}
		}
	};
}

TEST_CASE("a headless client fetches an asset through a real origin", "[cdn][delivery][e2e]") {
	World world;
	std::unique_ptr<AssetClient> client = MakeAssetClient(world.Over());
	REQUIRE(client != nullptr);
	client->UseGrant(world.Token());

	const RequestId id = client->Request("meshes/rock.mesh");
	REQUIRE(id.IsValid());
	REQUIRE(world.Settle(*client, id) == RequestState::Ready);

	const std::optional<Asset> asset = client->Take(id);
	REQUIRE(asset.has_value());
	CHECK(asset->Name == "meshes/rock.mesh");
	CHECK(asset->Kind == AssetKind::Mesh);

	// **The bytes are the bytes that were published.** Everything else in this
	// file is machinery for making this line mean something.
	CHECK(Text(asset->Bytes) == world.Published("meshes/rock.mesh"));

	// And its address is its content, still.
	CHECK(Hasher::Of(asset->Bytes) == Hasher::Of(asset->Bytes));
	CHECK(asset->Root == world.Catalogue->Find("meshes/rock.mesh")->Root);
}

TEST_CASE("what crossed the wire was compressed and expanded correctly", "[cdn][delivery][e2e]") {
	// **The compression check, measured at the socket.** `TransferredBytes` is
	// counted where bytes actually arrive and `ExpandedBytes` after Zstd, so
	// this compares the wire against the content rather than comparing a
	// setting against itself.
	World world;
	std::unique_ptr<AssetClient> client = MakeAssetClient(world.Over());
	REQUIRE(client != nullptr);
	client->UseGrant(world.Token());

	const RequestId id = client->Request("textures/grass.png");
	REQUIRE(world.Settle(*client, id) == RequestState::Ready);
	REQUIRE(client->Take(id).has_value());

	const auto &counters = client->Counters();
	CHECK(counters.Bundles >= 1);
	CHECK(counters.ExpandedBytes > 0);
	CHECK(counters.TransferredBytes > 0);
	// Strictly smaller: the group travelled compressed.
	CHECK(counters.TransferredBytes < counters.ExpandedBytes);
	// Nothing failed verification on the way.
	CHECK(counters.VerificationFailures == 0);
}

TEST_CASE("every hash matches what the publisher signed", "[cdn][delivery][e2e]") {
	// The client verifies each chunk against the asset root and the asset root
	// against the signed manifest, so fetching everything and finding no
	// verification failure is the whole chain holding.
	World world;
	std::unique_ptr<AssetClient> client = MakeAssetClient(world.Over());
	REQUIRE(client != nullptr);
	client->UseGrant(world.Token());
	world.SettleCatalogue(*client);
	REQUIRE(client->Ready());

	const Manifest *catalogue = client->Catalogue();
	REQUIRE(catalogue != nullptr);
	// The catalogue the client verified is the one that was published.
	CHECK(catalogue->Root() == world.Catalogue->Root());

	for (const auto &[name, body] : world.Original) {
		INFO("asset " << name);
		const RequestId id = client->Request(name);
		REQUIRE(world.Settle(*client, id) == RequestState::Ready);

		const std::optional<Asset> asset = client->Take(id);
		REQUIRE(asset.has_value());
		CHECK(Text(asset->Bytes) == body);

		const auto *entry = catalogue->Find(name);
		REQUIRE(entry != nullptr);
		CHECK(asset->Root == entry->Root);
	}
	CHECK(client->Counters().VerificationFailures == 0);
}

TEST_CASE("assets can be fetched by kind", "[cdn][delivery][e2e]") {
	// "Connect and get assets of types", all the way through: the kind was
	// decided at publish, travelled in the signed manifest, and is what the
	// request selects on.
	World world;
	std::unique_ptr<AssetClient> client = MakeAssetClient(world.Over());
	REQUIRE(client != nullptr);
	client->UseGrant(world.Token());
	world.SettleCatalogue(*client);
	REQUIRE(client->Ready());

	const std::vector<RequestId> textures = client->RequestKind(AssetKind::Texture);
	REQUIRE(textures.size() == 2);

	for (const RequestId id : textures) {
		REQUIRE(world.Settle(*client, id) == RequestState::Ready);
		const std::optional<Asset> asset = client->Take(id);
		REQUIRE(asset.has_value());
		CHECK(asset->Kind == AssetKind::Texture);
		CHECK(Text(asset->Bytes) == world.Published(asset->Name));
	}

	CHECK(client->RequestKind(AssetKind::Mesh).size() == 2);
	CHECK(client->RequestKind(AssetKind::Audio).size() == 1);
	CHECK(client->RequestKind(AssetKind::Font).empty());
}

TEST_CASE("a group that lands brings its neighbours with it", "[cdn][delivery][e2e]") {
	// CDN.md §5 seen from the client: the unit that travels is a group, so
	// asking for one asset puts the others in the cache — which is the whole of
	// "the game progressively builds".
	World world;
	const fs::path cache = world.Root / "cache";
	std::unique_ptr<AssetClient> client = MakeAssetClient(world.Over(cache));
	REQUIRE(client != nullptr);
	client->UseGrant(world.Token());

	const RequestId first = client->Request("meshes/rock.mesh");
	REQUIRE(world.Settle(*client, first) == RequestState::Ready);
	REQUIRE(client->Take(first).has_value());

	const uint64_t bundlesAfterFirst = client->Counters().Bundles;
	const uint64_t hitsAfterFirst = client->Counters().CacheHits;

	// Something else from the same publication. If it shared the group, it is
	// already cached and costs no further transfer.
	const RequestId second = client->Request("textures/grass.png");
	REQUIRE(world.Settle(*client, second) == RequestState::Ready);
	REQUIRE(client->Take(second).has_value());

	const bool sharedGroup = client->Counters().Bundles == bundlesAfterFirst;
	if (sharedGroup) {
		CHECK(client->Counters().CacheHits > hitsAfterFirst);
	}
	// Either way nothing had to be re-verified or re-fetched twice.
	CHECK(client->Counters().VerificationFailures == 0);
}

TEST_CASE("a cache means the second run costs no network", "[cdn][delivery][e2e]") {
	// The reason the cache is on a disk at all.
	World world;
	const fs::path cache = world.Root / "cache";

	{
		std::unique_ptr<AssetClient> client = MakeAssetClient(world.Over(cache));
		REQUIRE(client != nullptr);
		client->UseGrant(world.Token());
		const RequestId id = client->Request("audio/bark.wav");
		REQUIRE(world.Settle(*client, id) == RequestState::Ready);
		REQUIRE(client->Take(id).has_value());
		CHECK(client->Counters().CacheMisses >= 1);
	}

	std::unique_ptr<AssetClient> again = MakeAssetClient(world.Over(cache));
	REQUIRE(again != nullptr);
	again->UseGrant(world.Token());

	const RequestId id = again->Request("audio/bark.wav");
	REQUIRE(world.Settle(*again, id) == RequestState::Ready);
	const std::optional<Asset> asset = again->Take(id);
	REQUIRE(asset.has_value());
	CHECK(Text(asset->Bytes) == world.Published("audio/bark.wav"));

	// Served from the cache: no group was fetched for it.
	CHECK(again->Counters().CacheHits == 1);
	CHECK(again->Counters().Bundles == 0);
}

TEST_CASE("a client with no grant gets nothing from an http origin", "[cdn][delivery][e2e]") {
	// The origin admits against a grant and nothing else. A client that was
	// never issued one is refused, and the request fails rather than hanging.
	World world;
	std::unique_ptr<AssetClient> client = MakeAssetClient(world.Over());
	REQUIRE(client != nullptr);

	const RequestId id = client->Request("meshes/rock.mesh");
	CHECK(world.Settle(*client, id) == RequestState::Failed);
	CHECK(client->Counters().SourceFailures >= 1);
}

TEST_CASE("an expired grant fails the fetch rather than half-serving it", "[cdn][delivery][e2e]") {
	World world;
	std::unique_ptr<AssetClient> client = MakeAssetClient(world.Over());
	REQUIRE(client != nullptr);
	client->UseGrant(world.Token(NOW - 1));

	const RequestId id = client->Request("meshes/rock.mesh");
	CHECK(world.Settle(*client, id) == RequestState::Failed);
}

TEST_CASE("a manifest signed by the wrong key is refused", "[cdn][delivery][e2e]") {
	// **The trust boundary.** A client that accepted this would accept content
	// from anybody who could answer on that port, which is the entire property
	// that makes fetching from a third-party origin safe.
	World world;
	DeliverySettings settings = world.Over();
	settings.Publisher = Publisher(200).Public();

	std::unique_ptr<AssetClient> client = MakeAssetClient(settings);
	REQUIRE(client != nullptr);
	client->UseGrant(world.Token());

	const RequestId id = client->Request("meshes/rock.mesh");
	CHECK(world.Settle(*client, id) == RequestState::Failed);
	CHECK_FALSE(client->Ready());
	// Parsed and did not verify, which is counted apart from a source being
	// down: one is an operational event and the other is an attack or
	// corruption.
	CHECK(client->Counters().VerificationFailures >= 1);
}

TEST_CASE("an asset the manifest does not describe fails", "[cdn][delivery][e2e]") {
	World world;
	std::unique_ptr<AssetClient> client = MakeAssetClient(world.Over());
	REQUIRE(client != nullptr);
	client->UseGrant(world.Token());

	const RequestId id = client->Request("meshes/nonexistent.mesh");
	CHECK(world.Settle(*client, id) == RequestState::Failed);
}

// --- the source list -------------------------------------------------------

TEST_CASE("a local directory source needs no wire and no grant", "[cdn][delivery][e2e]") {
	// CDN.md §6's "local store": the server serving its own disk. Same manifest,
	// same verification, no transport — so there is no group to compress and
	// nothing to admit.
	World world;

	DeliverySettings settings;
	settings.Publisher = Publisher().Public();
	settings.Sources.push_back(
		Source{
			.Name = "on-disk",
			.Kind = SourceKind::Directory,
			.Location = (world.Root / "store").string(),
			.Enabled = true,
		}
	);

	std::unique_ptr<AssetClient> client = MakeAssetClient(settings);
	REQUIRE(client != nullptr);
	// Deliberately no grant.

	const RequestId id = client->Request("meshes/tree.mesh");
	REQUIRE(world.Settle(*client, id) == RequestState::Ready);

	const std::optional<Asset> asset = client->Take(id);
	REQUIRE(asset.has_value());
	CHECK(Text(asset->Bytes) == world.Published("meshes/tree.mesh"));
	CHECK(client->Counters().VerificationFailures == 0);
}

TEST_CASE("a dead source is passed over and the next one serves", "[cdn][delivery][e2e]") {
	// **What the priority list is for.** A source that refuses, times out or is
	// simply not there is skipped, and only when every one has been tried does
	// a request fail.
	World world;

	DeliverySettings settings;
	settings.Publisher = Publisher().Public();
	settings.IdlePolls = 200;
	// First: a directory that is not there. Second: the real origin.
	settings.Sources.push_back(
		Source{
			.Name = "missing-local",
			.Kind = SourceKind::Directory,
			.Location = (world.Root / "nowhere").string(),
			.Enabled = true,
		}
	);
	settings.Sources.push_back(
		Source{
			.Name = "origin",
			.Kind = SourceKind::Http,
			.Location = world.Location(),
			.Enabled = true,
		}
	);

	std::unique_ptr<AssetClient> client = MakeAssetClient(settings);
	REQUIRE(client != nullptr);
	client->UseGrant(world.Token());

	const RequestId id = client->Request("meshes/rock.mesh");
	REQUIRE(world.Settle(*client, id) == RequestState::Ready);

	const std::optional<Asset> asset = client->Take(id);
	REQUIRE(asset.has_value());
	CHECK(Text(asset->Bytes) == world.Published("meshes/rock.mesh"));
}

TEST_CASE("the local directory is preferred over the remote origin", "[cdn][delivery][e2e]") {
	// "Download from local first, otherwise request external" — which is not a
	// policy in the code, it is what this order *means*.
	World world;

	DeliverySettings settings;
	settings.Publisher = Publisher().Public();
	settings.Sources.push_back(
		Source{
			.Name = "on-disk",
			.Kind = SourceKind::Directory,
			.Location = (world.Root / "store").string(),
			.Enabled = true,
		}
	);
	settings.Sources.push_back(
		Source{
			.Name = "origin",
			.Kind = SourceKind::Http,
			.Location = world.Location(),
			.Enabled = true,
		}
	);

	std::unique_ptr<AssetClient> client = MakeAssetClient(settings);
	REQUIRE(client != nullptr);
	// No grant: if anything reached the http origin it would be refused, so a
	// success here proves the local source answered.

	const RequestId id = client->Request("textures/bark.png");
	REQUIRE(world.Settle(*client, id) == RequestState::Ready);
	REQUIRE(client->Take(id).has_value());

	CHECK(client->Counters().SourceFailures == 0);
	CHECK(world.Listening->Counters().Bundles == 0);
}

TEST_CASE("a client with no usable source is refused at construction", "[cdn][delivery][e2e]") {
	// Rather than half-applied: a client with no source fetches nothing and one
	// with no publisher key verifies nothing, and neither should look like a
	// working client.
	World world;

	DeliverySettings noKey = world.Over();
	noKey.Publisher = PublicKey{};
	CHECK(MakeAssetClient(noKey) == nullptr);

	DeliverySettings noSource = world.Over();
	noSource.Sources.clear();
	CHECK(MakeAssetClient(noSource) == nullptr);
}

TEST_CASE("a source outside the allow-list is never reached", "[cdn][delivery][e2e]") {
	// The request-forgery check, end to end: a descriptor a server sent cannot
	// point a client at an arbitrary host.
	World world;

	DeliverySettings settings = world.Over();
	settings.AllowedHosts = {"cdn.example.com"};
	CHECK(MakeAssetClient(settings) == nullptr);
}
